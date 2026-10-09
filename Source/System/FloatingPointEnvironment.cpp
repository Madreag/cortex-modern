#include "FloatingPointEnvironment.h"

#include <cfenv>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#elif !defined(__aarch64__)
#error "A floating-point environment policy is required for this architecture"
#endif

namespace RTE::FloatingPointEnvironment {
	namespace {
		thread_local bool s_Initialized = false;
#if defined(__aarch64__)
		constexpr uint64_t c_ControlMask = (3ULL << 22) | (1ULL << 24) | (1ULL << 19) | (3ULL << 25) | (0x1FULL << 8) | (1ULL << 15) | 3ULL;
		uint64_t ReadControl() { uint64_t control; asm volatile("mrs %0, fpcr" : "=r"(control)); return control; }
		void WriteControl(uint64_t control) { asm volatile("msr fpcr, %0\n\tisb" :: "r"(control) : "memory"); }
#else
		constexpr uint32_t c_ControlMask = 0xFFC0U;
		constexpr uint32_t c_ControlValue = 0x1F80U;
		uint32_t ReadControl() { return _mm_getcsr(); }
		void WriteControl(uint32_t control) { _mm_setcsr(control); }
#endif
	}

	bool IsValid() {
#if defined(__aarch64__)
		return std::fegetround() == FE_TONEAREST && (ReadControl() & c_ControlMask) == 0;
#else
		return std::fegetround() == FE_TONEAREST && (ReadControl() & c_ControlMask) == c_ControlValue;
#endif
	}

	void Assert(const char* boundary) {
		if (!IsValid()) {
			std::fprintf(stderr, "[fp-environment] invalid at %s round=%d control=%llx\n", boundary, std::fegetround(), static_cast<unsigned long long>(ReadControl()));
			std::abort();
		}
	}

	void Initialize() {
		if (std::fesetenv(FE_DFL_ENV) != 0 || std::fesetround(FE_TONEAREST) != 0) {
			std::fputs("[fp-environment] initialization failed\n", stderr);
			std::abort();
		}
#if defined(__aarch64__)
		WriteControl(ReadControl() & ~c_ControlMask);
#else
		WriteControl((ReadControl() & ~c_ControlMask) | c_ControlValue);
#endif
		s_Initialized = true;
		Assert("thread startup");
	}

	void Enter() {
		if (!s_Initialized) { Initialize(); }
		else { Assert("thread entry"); }
	}

	bool RunSelfTest() {
		bool passed = true;
		const auto check = [&passed](bool ok, const char* name) {
			std::printf("[fp-environment-selftest] %s %s\n", ok ? "PASS" : "FAIL", name);
			passed = passed && ok;
		};
		std::fesetround(FE_DOWNWARD);
		check(!IsValid(), "detect_rounding_drift");
		Initialize();
#if defined(__aarch64__)
		WriteControl(ReadControl() | (1ULL << 24));
#else
		WriteControl(ReadControl() | 0x8040U);
#endif
		check(!IsValid(), "detect_denormal_drift");
		std::atomic<bool> workerValid{false};
		auto worker = StartThread([&workerValid] { workerValid = IsValid(); });
		worker.join();
		check(workerValid.load(), "worker_pins_inherited_drift");
		Initialize();
		volatile float small = std::numeric_limits<float>::min();
		volatile float half = 0.5F;
		check(small * half == std::numeric_limits<float>::min() / 2.0F, "gradual_underflow");
		check(IsValid(), "startup_policy");
		std::printf("[fp-environment-selftest] %s\n", passed ? "PASS" : "FAIL");
		return passed;
	}
}
