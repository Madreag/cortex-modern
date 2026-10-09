#pragma once

#include <functional>
#include <future>
#include <thread>
#include <utility>

namespace RTE::FloatingPointEnvironment {

	void Initialize();
	bool IsValid();
	void Assert(const char* boundary);
	void Enter();
	bool RunSelfTest();

	class Scope {
	public:
		explicit Scope(const char* boundary) : m_Boundary(boundary) { Enter(); Assert(boundary); }
		~Scope() { Assert(m_Boundary); }
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;
	private:
		const char* m_Boundary;
	};

	template<class Function, class... Args>
	std::thread StartThread(Function&& function, Args&&... args) {
		return std::thread([work = std::bind(std::forward<Function>(function), std::forward<Args>(args)...)]() mutable {
			Initialize();
			const Scope scope("worker return");
			work();
		});
	}

	template<class Function, class... Args>
	auto Async(std::launch policy, Function&& function, Args&&... args) {
		return std::async(policy, [work = std::bind(std::forward<Function>(function), std::forward<Args>(args)...)]() mutable -> decltype(auto) {
			const Scope scope("async return");
			return work();
		});
	}
}
