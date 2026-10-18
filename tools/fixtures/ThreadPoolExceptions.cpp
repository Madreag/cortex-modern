// The ordinary build uses byte-identical production headers. The injected build
// changes only exception capture and the call boundary of promise::set_exception.
// It models a runtime that cannot represent a caught foreign exception without
// invoking the undefined set_exception(nullptr) operation on the old code.
#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <new>
#include <string_view>

namespace FailureProbe {
    inline thread_local bool emptyCapture = false;
    inline std::atomic<unsigned> emptied{0}, nullPromises{0}, wrongThrow{0};
    inline const std::exception_ptr fallback = std::make_exception_ptr(std::bad_exception{});

    std::exception_ptr CurrentException() noexcept {
        auto error = std::current_exception();
        if (!emptyCapture) return error;
        try {
            if (!error) { ++wrongThrow; return {}; }
            std::rethrow_exception(error);
        } catch (const std::bad_alloc&) {
            ++emptied;
        } catch (...) {
            ++wrongThrow;
        }
        return {};
    }

    template<class T>
    void SetException(std::promise<T>& promise, std::exception_ptr error) {
        if (!error) {
            ++nullPromises;
            // Record the violated precondition, then safely let the worker finish.
            // The test still fails; no undefined STL behavior decides the verdict.
            error = fallback;
        }
        promise.set_exception(error);
    }
}

#include "BS_thread_pool.hpp"
#include "BS_thread_pool_light.hpp"

template<class T>
bool CheckFuture(std::future<T>& future, bool injected) {
    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) return false;
    try { (void)future.get(); }
    catch (const std::bad_alloc&) { return !injected; }
    catch (const std::bad_exception&) { return injected; }
    catch (...) { return false; }
    return false;
}

template<class Pool>
bool CheckPool(std::string_view name) {
    Pool pool(2);
    bool all = true;
#ifdef INJECT_EMPTY_EXCEPTION
    constexpr int modes = 2;
#else
    constexpr int modes = 1;
#endif
    for (int mode = 0; mode < modes; ++mode) {
        const bool injected = mode != 0;
        const auto before = FailureProbe::nullPromises.load();
        auto empty = pool.submit([injected] {
            FailureProbe::emptyCapture = injected;
            throw std::bad_alloc{};
        });
        auto value = pool.submit([injected]() -> int {
            FailureProbe::emptyCapture = injected;
            throw std::bad_alloc{};
        });
        const bool voidOk = CheckFuture(empty, injected);
        const bool valueOk = CheckFuture(value, injected);
        auto next = pool.submit([] { FailureProbe::emptyCapture = false; return 73; });
        const bool continued = next.wait_for(std::chrono::seconds(2)) == std::future_status::ready && next.get() == 73;
        const bool noNull = FailureProbe::nullPromises.load() == before;
        const bool ok = voidOk && valueOk && continued && noNull;
        std::cout << "[pool-exception] pool=" << name << " injected=" << injected
                  << " void=" << voidOk << " value=" << valueOk << " continued=" << continued
                  << " nonnull=" << noNull << " pass=" << ok << std::endl;
        all &= ok;
    }
    return all;
}

int main() {
    const bool full = CheckPool<BS::thread_pool>("full");
    const bool light = CheckPool<BS::thread_pool_light>("light");
#ifdef INJECT_EMPTY_EXCEPTION
    const bool probed = FailureProbe::emptied == 4 && FailureProbe::wrongThrow == 0;
#else
    const bool probed = FailureProbe::emptied == 0 && FailureProbe::wrongThrow == 0;
#endif
    const bool passed = full && light && probed && FailureProbe::nullPromises == 0;
    std::cout << "[pool-exception] emptied=" << FailureProbe::emptied << " null_promises=" << FailureProbe::nullPromises
              << " wrong_throw=" << FailureProbe::wrongThrow << " pass=" << passed << std::endl;
    return passed ? 0 : 1;
}
