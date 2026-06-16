#include <thread>
#include <iostream>

#include "spsc_ring_buffer.hpp"

int main() {
    MutexRingBuffer<int, 8> queue;

    std::thread producer([&] {
        for (int i = 1; i <= 20; ++i) {
            while (!queue.push(i)) {
                // queue full, retry
            }
        }
    });

    std::thread consumer([&] {
        int received = 0;

        while (received < 20) {
            auto value = queue.pop();

            if (value) {
                std::cout << *value << '\n';
                ++received;
            }
        }
    });

    producer.join();
    consumer.join();
}