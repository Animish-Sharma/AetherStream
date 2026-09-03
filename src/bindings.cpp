#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include <cstring>

#include "aether/aether.hpp"
#include "aether/ged_estimator.hpp"
#include "aether/rate_controller.hpp"
#include "aether/stream_encoder.hpp"
#include "aether/table.hpp"

namespace py = pybind11;

namespace {

void require_vector(const py::buffer_info& buffer, const char* name) {
    if (buffer.ndim != 1)
        throw py::value_error(std::string(name) + " must be a one-dimensional array");
}

py::bytes as_bytes(const std::vector<uint8_t>& bytes) {
    return py::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}  // namespace

PYBIND11_MODULE(aether, module) {
    module.doc() = "AetherStream v2.2 indexed telemetry compression";
    module.attr("__version__") = "2.2.0";
    py::register_exception<aether::CorruptedStreamException>(module, "CorruptedStreamError",
                                                             PyExc_RuntimeError);
    py::register_exception<aether::RateBudgetExceeded>(module, "RateBudgetError",
                                                       PyExc_RuntimeError);

    module.def(
        "compress",
        [](py::array_t<float, py::array::c_style | py::array::forcecast> input, float target_rate,
           float absolute_error, float deadzone, bool enable_index) {
            const py::buffer_info buffer = input.request();
            require_vector(buffer, "samples");
            const auto* samples = static_cast<const float*>(buffer.ptr);
            std::vector<uint8_t> encoded;
            {
                py::gil_scoped_release release;
                encoded = aether::AetherCodec(target_rate, deadzone, absolute_error, enable_index)
                              .compress(std::span<const float>(samples, buffer.size));
            }
            return as_bytes(encoded);
        },
        py::arg("samples"), py::arg("target_rate") = 4.0f, py::arg("absolute_error") = 0.0f,
        py::arg("deadzone") = 0.3f, py::arg("enable_index") = false);

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
            if (source.itemsize != 1 || source.ndim != 1 || source.strides[0] != 1)
                throw py::value_error("compressed data must be a contiguous byte buffer");
            if (output.itemsize != sizeof(float) || output.ndim != 1 ||
                output.strides[0] != static_cast<py::ssize_t>(sizeof(float)) ||
                static_cast<std::size_t>(output.size) != length)
                throw py::value_error("destination must be a contiguous float32 buffer");
            {
                py::gil_scoped_release release;
                aether::AetherCodec().decompress(
                    std::span<const uint8_t>(static_cast<const uint8_t*>(source.ptr), source.size),
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

    module.def(
        "estimate_ged",
        [](py::array_t<float, py::array::c_style | py::array::forcecast> input) {
            const py::buffer_info buffer = input.request();
            require_vector(buffer, "residuals");
            const auto* values = static_cast<const float*>(buffer.ptr);
            aether::GedParameters ged;
            {
                py::gil_scoped_release release;
                ged = aether::GedEstimator().estimate(values, buffer.size);
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
                 require_vector(buffer, "chunk");
                 const auto* samples = static_cast<const float*>(buffer.ptr);
                 std::vector<uint8_t> encoded;
                 {
                     py::gil_scoped_release release;
                     encoded = encoder.feed(std::span<const float>(samples, buffer.size));
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
