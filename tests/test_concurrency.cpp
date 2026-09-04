#include <atomic>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "aether/aether.hpp"
#include "aether/stream_encoder.hpp"

int main() {
    constexpr unsigned thread_count = 16;
    constexpr std::size_t sample_count = 1'000'000;
    std::atomic<bool> failed{false};
    std::mutex diagnostics;
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (unsigned worker = 0; worker < thread_count; ++worker) {
        workers.emplace_back([worker, &failed, &diagnostics] {
            try {
                std::vector<float> samples(sample_count);
                for (std::size_t i = 0; i < samples.size(); ++i) {
                    const float phase =
                        static_cast<float>(i) * (0.002f + static_cast<float>(worker) * 0.0001f);
                    samples[i] = std::sin(phase) + 0.01f * std::cos(17.0f * phase);
                }

                aether::AetherCodec codec(4.0f, 0.3f, 0.001f);
                const auto encoded = codec.compress(samples);
                std::vector<float> batch(samples.size());
                codec.decompress(encoded, batch);
                for (std::size_t i = 0; i < samples.size(); ++i) {
                    if (std::abs(samples[i] - batch[i]) > 0.001001f)
                        throw std::runtime_error("concurrent error-bound violation");
                }

                aether::StreamEncoder encoder(4.0f, 0.3f, 0.001f);
                aether::StreamDecoder decoder;
                std::vector<float> streamed;
                for (std::size_t offset = 0; offset < samples.size(); offset += 173) {
                    const std::size_t count = std::min<std::size_t>(173, samples.size() - offset);
                    const auto bytes =
                        encoder.feed(std::span<const float>(samples.data() + offset, count));
                    const auto decoded = decoder.feed(bytes);
                    streamed.insert(streamed.end(), decoded.begin(), decoded.end());
                }
                const auto tail = encoder.flush();
                const auto decoded_tail = decoder.feed(tail);
                streamed.insert(streamed.end(), decoded_tail.begin(), decoded_tail.end());
                decoder.flush();
                if (streamed != batch) throw std::runtime_error("concurrent stream mismatch");
            } catch (const std::exception& error) {
                std::lock_guard<std::mutex> lock(diagnostics);
                std::cerr << "worker " << worker << ": " << error.what() << '\n';
                failed.store(true, std::memory_order_relaxed);
            } catch (...) {
                std::lock_guard<std::mutex> lock(diagnostics);
                std::cerr << "worker " << worker << ": unknown exception\n";
                failed.store(true, std::memory_order_relaxed);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    return failed.load(std::memory_order_relaxed) ? 1 : 0;
}
