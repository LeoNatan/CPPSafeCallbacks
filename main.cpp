//
//  main.cpp
//  SafeCallbacks
//
//  Created by Léo Natan on 8/9/24.
//

#define SAFE_CALLBACKS_DEBUG_PRINTS 1

#include "SafeCallbacks.hpp"

#include <functional>
#include <memory>
#include <print>
#include <thread>
#include <chrono>

/*
 RELEASE_AFTER = release owner after callbacks have finished
 RELEASE_BEFORE = release owner before any callback has been called
 RELEASE_DURING = release owner while callbacks are running
 RELEASE_INSIDE = release owner from inside a callback
 */

#define RELEASE_AFTER 0
#define RELEASE_BEFORE 1
#define RELEASE_DURING 2
#define RELEASE_INSIDE 3

#if !defined(RELEASE_BEFORE_OR_DURING_CALL)
#define RELEASE_BEFORE_OR_DURING_CALL RELEASE_AFTER
#endif

#if __cplusplus <= 202002L
#if __cplusplus == 202002L
#include <format>
#include <iostream>

namespace std {

template <typename... Args>
void println(std::format_string<Args...> fmt, Args&&... args) {
	std::cout << std::format(fmt, std::forward<Args>(args)...) << endl;
}

}

#else
#include <iostream>
namespace std {

template<typename ...Args>
void println(const char* fmt, Args&&... args) {
	std::cout << fmt;
	((std::cout << ' ' << std::forward<Args>(args)), ...);
	std::cout << std::endl;
}

}
#endif
#endif

class non_default_constructible
{
public:
	non_default_constructible() = delete;
	non_default_constructible(const non_default_constructible&) = delete;
	non_default_constructible(non_default_constructible&&) = default;
	non_default_constructible(bool) {};
};

class is_it_safe
{
public:
	is_it_safe(): some(std::make_unique<std::string>("ACCESSING POINTER MEMBER OF is_it_safe")) {}
	
	const std::string& get_string()
	{
		return *some;
	}
	
	static void static_member_func()
	{
		std::println("From static_member_func");
	}
	
	void member_func()
	{
		std::println("From member_func");
	}
	
	void member_func_const() const
	{
		std::println("From member_func_const");
	}
	
	auto make_safe(auto&& c, const char*&& name = "")
	{
		return cb.make_safe(std::forward<decltype(c)>(c), std::forward<const char*>(name));
	}
	
	auto make_safe(auto&& r, auto&& c, const char*&& name = "")
	{
		return cb.make_safe(std::forward<decltype(r)>(r), std::forward<decltype(c)>(c), std::forward<const char*>(name));
	}
	
private:
	std::unique_ptr<std::string> some;
	
	//safe_callback instances should be placed at the bottom of the members list.
	safe_callbacks cb;
};

std::string string_returning_func(double asd)
{
	std::println("Hello from string_returning_func!");
	return "from function";
}

class recursion_helper
{
public:
	std::function<void(int)> func;
};

std::thread test(is_it_safe* owner)
{
	auto void_callback = owner->make_safe([owner]() {
#if RELEASE_BEFORE_OR_DURING_CALL == RELEASE_DURING
		std::println("Sleeping for 2 seconds inside void_callback");
		std::this_thread::sleep_for (std::chrono::seconds(2));
#endif
		std::println("void_callback: {0}", owner->get_string().c_str());
#if RELEASE_BEFORE_OR_DURING_CALL == RELEASE_INSIDE
		std::println("void_callback: Deleting owner from inside void_callback");
		delete owner;
#endif
	}, "void_callback");
	auto static_member_func_callback = owner->make_safe(&is_it_safe::static_member_func, "static_member_func_callback");
	std::function<void(void)> member_func = std::bind(&is_it_safe::member_func, owner);
	auto member_func_callback = owner->make_safe(member_func, "member_func_callback");
	std::function<void(void)> member_func_const = std::bind(&is_it_safe::member_func_const, owner);
	auto member_func_const_callback = owner->make_safe(member_func_const, "member_func_const_callback");
	auto str_callback = owner->make_safe("cancelled default value", string_returning_func, "str_callback");
//	auto str_callback_ = owner->make_safe(123, string_returning_func); // <- This should produce an error!
	auto default_return_val = owner->make_safe([] {
		return std::string("lambda return value");
	}, "default_return_val");
	auto non_default_constructible_callback = owner->make_safe(non_default_constructible(false), [] { return non_default_constructible(true); }, "non_default_constructible_callback");
//	auto non_default_constructible_callback_ = owner->make_safe([] { return non_default_constructible(true); }); // <- This should produce an error!
	
	auto rec = new recursion_helper();
	auto recursive_callback = rec->func = owner->make_safe([rec, owner](int count) mutable {
		std::println("recursive count {0}", count);
		if(count == 0)
		{
			delete rec;
			return;
		}
		rec->func(count - 1);
	}, "recursive_callback");
	
	return std::thread([=] {
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		
		void_callback();
		static_member_func_callback();
		member_func_callback();
		member_func_const_callback();
		std::println("str_callback: {0}", str_callback([]{return 3.0;}()));
		std::println("default_return_val: {0}", default_return_val());
		non_default_constructible_callback();
		recursive_callback(3);
	});
}

int main(int argc, const char * argv[]) {
	auto owner = new is_it_safe();
	
	std::thread call_callbacks = test(owner);
	
	auto delete_owner_f = [owner] {
#if RELEASE_BEFORE_OR_DURING_CALL != RELEASE_INSIDE
#if RELEASE_BEFORE_OR_DURING_CALL != RELEASE_BEFORE
		std::this_thread::sleep_for(
#if RELEASE_BEFORE_OR_DURING_CALL == RELEASE_AFTER
									std::chrono::seconds(2)
#else
									std::chrono::seconds(1)
#endif
									);
#endif
		
		delete owner;
#endif
	};

#if RELEASE_BEFORE_OR_DURING_CALL == RELEASE_BEFORE || RELEASE_BEFORE_OR_DURING_CALL == RELEASE_INSIDE
	delete_owner_f();
	std::thread delete_owner([]{});
#else
	std::thread delete_owner(delete_owner_f);
#endif
	
	call_callbacks.join();
	delete_owner.join();
	
	return 0;
}

