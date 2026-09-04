#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstring>
#include <memory>

#include "aether/aether.hpp"
#include "aether/eclm_quantizer.hpp"
#include "aether/ged_estimator.hpp"
#include "aether/predictor.hpp"
#include "aether/rate_controller.hpp"
#include "aether/stream_encoder.hpp"
#include "aether/table.hpp"

namespace py = pybind11;

namespace {

std::size_t require_vector(const py::buffer_info& buffer, const char* name) {
    if (buffer.ndim != 1)
        throw py::value_error(std::string(name) + " must be a one-dimensional array");
    if (buffer.size < 0) throw py::value_error(std::string(name) + " has an invalid size");
    return static_cast<std::size_t>(buffer.size);
}

py::bytes as_bytes(const std::vector<uint8_t>& bytes) {
    return py::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

class PyIndexedStreamView {
   public:
    explicit PyIndexedStreamView(py::bytes encoded) : owner_(std::move(encoded)) {
        char* raw = nullptr;
        py::ssize_t size = 0;
        if (PyBytes_AsStringAndSize(owner_.ptr(), &raw, &size) != 0) throw py::error_already_set();
        view_ = std::make_unique<aether::IndexedStreamView>(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(raw), static_cast<std::size_t>(size)));
    }

    py::array_t<float> decompress_slice(std::size_t start, std::size_t count) const {
        py::array_t<float> output(count);
        {
            py::gil_scoped_release release;
            view_->decompress_slice(start, count, std::span<float>(output.mutable_data(), count));
        }
        return output;
    }

    std::size_t sample_count() const noexcept { return view_->sample_count(); }
    std::size_t block_count() const noexcept { return view_->block_count(); }

   private:
    py::bytes owner_;
    std::unique_ptr<aether::IndexedStreamView> view_;
};

class QuantizerBenchmark {
   public:
    QuantizerBenchmark(float rate, float alpha, float beta, float peak) : quantizer_(rate) {
        quantizer_.design(aether::GedParameters{0.0f, alpha, beta}, 40, 0.0f, peak);
    }

    void fit(py::array_t<float, py::array::c_style | py::array::forcecast> input) {
        const py::buffer_info buffer = input.request();
        const std::size_t size = require_vector(buffer, "samples");
        py::gil_scoped_release release;
        quantizer_.fit_reconstruction_samples(static_cast<const float*>(buffer.ptr), size);
    }

    py::array_t<uint8_t> quantize(
        py::array_t<float, py::array::c_style | py::array::forcecast> input,
        const std::string& backend) const {
        const py::buffer_info buffer = input.request();
        const std::size_t size = require_vector(buffer, "samples");
        const auto* values = static_cast<const float*>(buffer.ptr);
        py::array_t<uint8_t> output(size);
        {
            py::gil_scoped_release release;
            if (backend == "exact") {
                quantizer_.quantize_exact(values, output.mutable_data(), size);
            } else {
                quantizer_.quantize_polynomial_backend(values, output.mutable_data(), size,
                                                       parse_backend(backend));
            }
        }
        return output;
    }

    py::array_t<float> reconstruct(
        py::array_t<uint8_t, py::array::c_style | py::array::forcecast> symbols) const {
        const py::buffer_info buffer = symbols.request();
        const std::size_t size = require_vector(buffer, "symbols");
        const auto* codes = static_cast<const uint8_t*>(buffer.ptr);
        py::array_t<float> output(size);
        for (std::size_t i = 0; i < size; ++i)
            output.mutable_data()[i] = quantizer_.reconstruct(codes[i]);
        return output;
    }

   private:
    static aether::simd::Backend parse_backend(const std::string& name) {
        using aether::simd::Backend;
        if (name == "scalar") return Backend::SCALAR;
        if (name == "native") return aether::simd::detected_backend();
        if (name == "avx2" && aether::simd::backend_available(Backend::AVX2)) return Backend::AVX2;
        if (name == "avx512" && aether::simd::backend_available(Backend::AVX512))
            return Backend::AVX512;
        if (name == "neon" && aether::simd::backend_available(Backend::NEON)) return Backend::NEON;
        throw py::value_error("requested SIMD backend is unavailable");
    }

    aether::ECLMQuantizer quantizer_;
};

const char* backend_name(aether::simd::Backend backend) {
    using aether::simd::Backend;
    switch (backend) {
        case Backend::NEON:
            return "neon";
        case Backend::AVX2:
            return "avx2";
        case Backend::AVX512:
            return "avx512";
        default:
            return "scalar";
    }
}

}  // namespace

PYBIND11_MODULE(aether, module) {
    module.doc() = "AetherStream 0.0.1 indexed telemetry compression";
    module.attr("__version__") = "0.0.1";
    const auto stream_error =
        py::register_exception<aether::StreamError>(module, "StreamError", PyExc_RuntimeError);
    const auto corrupted_error = py::register_exception<aether::CorruptedStreamException>(
        module, "CorruptedStreamError", stream_error.ptr());
    py::register_exception<aether::DeprecatedWireFormatException>(
        module, "DeprecatedWireFormatError", corrupted_error.ptr());
    py::register_exception<aether::UnsupportedWireFormatException>(
        module, "UnsupportedWireFormatError", corrupted_error.ptr());
    py::register_exception<aether::RateBudgetExceeded>(module, "RateBudgetError",
                                                       stream_error.ptr());

    module.def(
        "compress",
        [](py::array_t<float, py::array::c_style | py::array::forcecast> input, float target_rate,
           float absolute_error, float deadzone, bool enable_index, bool adaptive_tail) {
            const py::buffer_info buffer = input.request();
            const std::size_t size = require_vector(buffer, "samples");
            const auto* samples = static_cast<const float*>(buffer.ptr);
            std::vector<uint8_t> encoded;
            {
                py::gil_scoped_release release;
                encoded = aether::AetherCodec(target_rate, deadzone, absolute_error, enable_index,
                                              adaptive_tail)
                              .compress(std::span<const float>(samples, size));
            }
            return as_bytes(encoded);
        },
        py::arg("samples"), py::arg("target_rate") = 4.0f, py::arg("absolute_error") = 0.0f,
        py::arg("deadzone") = 0.3f, py::arg("enable_index") = false,
        py::arg("adaptive_tail") = true);

    module.def(
        "decompress",
        [](py::bytes encoded, std::size_t length) {
            char* raw = nullptr;
            py::ssize_t byte_count = 0;
            if (PyBytes_AsStringAndSize(encoded.ptr(), &raw, &byte_count) != 0)
                throw py::error_already_set();
            const auto* data = reinterpret_cast<const uint8_t*>(raw);
            py::array_t<float> output(length);
            {
                py::gil_scoped_release release;
                aether::AetherCodec().decompress(
                    std::span<const uint8_t>(data, static_cast<std::size_t>(byte_count)),
                    std::span<float>(output.mutable_data(), length));
            }
            return output;
        },
        py::arg("data"), py::arg("length"));

    module.def(
        "_decompress_into",
        [](py::buffer encoded, std::size_t length, py::buffer destination) {
            const py::buffer_info source = encoded.request();
            const py::buffer_info output = destination.request(true);
            if (source.itemsize != 1 || source.ndim != 1 || source.strides[0] != 1 ||
                source.size < 0)
                throw py::value_error("compressed data must be a contiguous byte buffer");
            if (output.itemsize != sizeof(float) || output.ndim != 1 || output.size < 0 ||
                output.strides[0] != static_cast<py::ssize_t>(sizeof(float)) ||
                static_cast<std::size_t>(output.size) != length)
                throw py::value_error("destination must be a contiguous float32 buffer");
            {
                py::gil_scoped_release release;
                aether::AetherCodec().decompress(
                    std::span<const uint8_t>(static_cast<const uint8_t*>(source.ptr),
                                             static_cast<std::size_t>(source.size)),
                    std::span<float>(static_cast<float*>(output.ptr), length));
            }
        },
        py::arg("data"), py::arg("length"), py::arg("destination"));

    module.def(
        "decompress_slice",
        [](py::bytes encoded, std::size_t start, std::size_t count) {
            char* raw = nullptr;
            py::ssize_t byte_count = 0;
            if (PyBytes_AsStringAndSize(encoded.ptr(), &raw, &byte_count) != 0)
                throw py::error_already_set();
            const auto* data = reinterpret_cast<const uint8_t*>(raw);
            py::array_t<float> output(count);
            {
                py::gil_scoped_release release;
                aether::decompress_slice(
                    std::span<const uint8_t>(data, static_cast<std::size_t>(byte_count)), start,
                    count, std::span<float>(output.mutable_data(), count));
            }
            return output;
        },
        py::arg("data"), py::arg("start"), py::arg("count"));

    py::class_<PyIndexedStreamView>(module, "IndexedStreamView")
        .def(py::init<py::bytes>(), py::arg("data"))
        .def_property_readonly("sample_count", &PyIndexedStreamView::sample_count)
        .def_property_readonly("block_count", &PyIndexedStreamView::block_count)
        .def("decompress_slice", &PyIndexedStreamView::decompress_slice, py::arg("start"),
             py::arg("count"));

    py::class_<QuantizerBenchmark>(module, "_QuantizerBenchmark")
        .def(py::init<float, float, float, float>(), py::arg("rate"), py::arg("alpha"),
             py::arg("beta"), py::arg("peak"))
        .def("fit", &QuantizerBenchmark::fit, py::arg("samples"))
        .def("quantize", &QuantizerBenchmark::quantize, py::arg("samples"), py::arg("backend"))
        .def("reconstruct", &QuantizerBenchmark::reconstruct, py::arg("symbols"));
    module.def("_simd_backend", []() { return backend_name(aether::simd::detected_backend()); });

    module.def(
        "_analyze_impulsive_blocks",
        [](py::array_t<float, py::array::c_style | py::array::forcecast> input) {
            const py::buffer_info buffer = input.request();
            const std::size_t size = require_vector(buffer, "samples");
            const auto* samples = static_cast<const float*>(buffer.ptr);
            std::size_t triggered = 0;
            std::size_t blocks = 0;
            std::vector<std::size_t> triggered_indices;
            float minimum_beta = 5.0f;
            double maximum_peak_sigma = 0.0;
            {
                py::gil_scoped_release release;
                aether::GedEstimator estimator;
                std::vector<float> residuals(aether::BLOCK_SIZE);
                for (std::size_t offset = 0; offset < size; offset += aether::BLOCK_SIZE) {
                    const std::size_t count = std::min(aether::BLOCK_SIZE, size - offset);
                    float parameter = 0.0f;
                    const auto mode =
                        aether::select_optimal_predictor(samples + offset, count, parameter);
                    aether::Predictor::residuals(samples + offset, residuals.data(), count, mode,
                                                 parameter);
                    const aether::GedParameters ged = estimator.estimate(residuals.data(), count);
                    float peak = 0.0f;
                    for (std::size_t i = 0; i < count; ++i)
                        peak = std::max(peak, std::abs(residuals[i]));
                    const double sigma = ged.alpha * std::sqrt(std::tgamma(3.0 / ged.beta) /
                                                               std::tgamma(1.0 / ged.beta));
                    const double peak_sigma = sigma > 0.0 ? peak / sigma : 0.0;
                    minimum_beta = std::min(minimum_beta, ged.beta);
                    maximum_peak_sigma = std::max(maximum_peak_sigma, peak_sigma);
                    if (ged.beta < 0.8f && peak_sigma > 8.0) {
                        ++triggered;
                        triggered_indices.push_back(blocks);
                    }
                    ++blocks;
                }
            }
            py::dict result;
            result["blocks"] = blocks;
            result["triggered_blocks"] = triggered;
            result["triggered_block_indices"] = triggered_indices;
            result["minimum_beta"] = minimum_beta;
            result["maximum_peak_sigma"] = maximum_peak_sigma;
            return result;
        },
        py::arg("samples"));

    module.def(
        "estimate_ged",
        [](py::array_t<float, py::array::c_style | py::array::forcecast> input) {
            const py::buffer_info buffer = input.request();
            const std::size_t size = require_vector(buffer, "residuals");
            const auto* values = static_cast<const float*>(buffer.ptr);
            aether::GedParameters ged;
            {
                py::gil_scoped_release release;
                ged = aether::GedEstimator().estimate(values, size);
            }
            return py::make_tuple(ged.alpha, ged.beta);
        },
        py::arg("residuals"));

    py::class_<aether::StreamEncoder>(module, "StreamEncoder")
        .def(py::init([](float target_rate, float absolute_error, float deadzone) {
                 return aether::StreamEncoder(target_rate, deadzone, absolute_error);
             }),
             py::arg("target_rate") = 4.0f, py::arg("absolute_error") = 0.0f,
             py::arg("deadzone") = 0.3f)
        .def("feed",
             [](aether::StreamEncoder& encoder,
                py::array_t<float, py::array::c_style | py::array::forcecast> input) {
                 const py::buffer_info buffer = input.request();
                 const std::size_t size = require_vector(buffer, "chunk");
                 const auto* samples = static_cast<const float*>(buffer.ptr);
                 std::vector<uint8_t> encoded;
                 {
                     py::gil_scoped_release release;
                     encoded = encoder.feed(std::span<const float>(samples, size));
                 }
                 return as_bytes(encoded);
             })
        .def("flush", [](aether::StreamEncoder& encoder) {
            std::vector<uint8_t> encoded;
            {
                py::gil_scoped_release release;
                encoded = encoder.flush();
            }
            return as_bytes(encoded);
        });

    py::class_<aether::StreamDecoder>(module, "StreamDecoder")
        .def(py::init<>())
        .def("feed",
             [](aether::StreamDecoder& decoder, py::bytes encoded) {
                 char* raw = nullptr;
                 py::ssize_t byte_count = 0;
                 if (PyBytes_AsStringAndSize(encoded.ptr(), &raw, &byte_count) != 0)
                     throw py::error_already_set();
                 const auto* data = reinterpret_cast<const uint8_t*>(raw);
                 std::vector<float> decoded;
                 {
                     py::gil_scoped_release release;
                     decoded = decoder.feed(
                         std::span<const uint8_t>(data, static_cast<std::size_t>(byte_count)));
                 }
                 py::array_t<float> output(decoded.size());
                 if (!decoded.empty())
                     std::memcpy(output.mutable_data(), decoded.data(),
                                 decoded.size() * sizeof(float));
                 return output;
             })
        .def("flush", &aether::StreamDecoder::flush);
}
