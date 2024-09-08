//
//  main.cpp
//  SafeCallbacks
//
//  Created by Léo Natan on 8/9/24.
//

#define SAFE_CALLBACKS_DEBUG_PRINTS 1

#include "SafeCallbacks.hpp"
#include <dispatch/dispatch.h>
#include <functional>
#include <memory>
#include <print>

#define RELEASE_BEFORE_CALL 0

class non_default_constructible
{
public:
	non_default_constructible() = delete;
	non_default_constructible(bool)
	{
		
	}
};

class is_it_safe: public safe_callbacks
{
public:
	std::string* some;
	
public:
	is_it_safe(): some(new std::string("ACCESSING POINTER MEMBER OF is_it_safe")) {}
	
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
};

class auto_dispatch_group
{
public:
	auto_dispatch_group()
	{
		shared_group.reset(dispatch_group_create(), [](dispatch_group_t group) {
			dispatch_release(group);
		});
	}
	auto_dispatch_group(const auto_dispatch_group& other)
	{
		shared_group = other.shared_group;
		dispatch_group_enter(*this);
	}
	~auto_dispatch_group()
	{
		dispatch_group_leave(*this);
	}
	auto_dispatch_group(auto_dispatch_group&&) = delete;
	auto_dispatch_group& operator=(auto_dispatch_group&&) = delete;
	
	operator dispatch_group_t() const { return shared_group.get(); }
	
private:
	std::shared_ptr<dispatch_group_s> shared_group;
};

auto make_scope_dispatch_group_leave(dispatch_group_t* group)
{
	return std::shared_ptr<dispatch_group_t>(group, [](dispatch_group_t* group) {
		dispatch_group_leave(*group);
	});
}

std::string string_returning_func(double asd)
{
	std::println("Hello from string_returning_func!");
	return "from function";
}

void test(auto_dispatch_group group)
{
	auto owner = new is_it_safe();
	
	auto void_callback = owner->make_safe([owner]() {
		std::println("{0}", owner->some->c_str());
	});
	auto static_member_func_callback = owner->make_safe(&is_it_safe::static_member_func);
	std::function<void(void)> member_func = std::bind(&is_it_safe::member_func, owner);
	auto member_funcCallback = owner->make_safe(member_func);
	std::function<void(void)> member_func_const = std::bind(&is_it_safe::member_func_const, owner);
	auto member_func_const_callback = owner->make_safe(member_func_const);
	auto str_callback = owner->make_safe("cancelled default value", string_returning_func);
//	auto str_callback = owner->make_safe(123, string_returning_func); // <- This should produce an error!
	auto default_return_val = owner->make_safe([] {
		return std::string("lambda return value");
	});
	auto non_default_constructible_callback = owner->make_safe(non_default_constructible(false), [] { return non_default_constructible(true); });
	
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^ {
		void_callback();
		static_member_func_callback();
		member_funcCallback();
		member_func_const_callback();
		std::println("str_callback: {0}", str_callback([]{return 3.0;}()));
		std::println("default_return_val: {0}", default_return_val());
		
		auto local = group;
	});
	
#if !RELEASE_BEFORE_CALL
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
#endif
		delete owner;
		
		auto local = group;
#if !RELEASE_BEFORE_CALL
	});
#endif
}

int main(int argc, const char * argv[]) {
	auto group = auto_dispatch_group();

	test(group);
	dispatch_group_notify(group, dispatch_get_main_queue(), ^{
		exit(0);
	});
	
	dispatch_main();
}

