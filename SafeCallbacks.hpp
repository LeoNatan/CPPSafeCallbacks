//
//  SafeCallbacks.hpp
//  SafeCallbacks
//
//  Created by Léo Natan on 05/09/2024.
//

#pragma once

#include <memory>
#include <variant>
#include <unordered_map>
#include <functional>
#include <mutex>

#if __cplusplus < 202302L
#undef SAFE_CALLBACKS_DEBUG_PRINTS
#define SAFE_CALLBACKS_DEBUG_PRINTS 0
#else
#ifndef SAFE_CALLBACKS_DEBUG_PRINTS
#define SAFE_CALLBACKS_DEBUG_PRINTS 0
#endif
#endif

#if SAFE_CALLBACKS_DEBUG_PRINTS
#include <print>
#endif

#if __APPLE__
#include <os/lock.h>

namespace {
class apple_unfair_lock
{
public:
	apple_unfair_lock(): _lock(OS_UNFAIR_LOCK_INIT) {}
	apple_unfair_lock(const apple_unfair_lock&) = delete;
	apple_unfair_lock& operator=(const apple_unfair_lock&) = delete;
	apple_unfair_lock(apple_unfair_lock&&) = delete;
	apple_unfair_lock& operator=(apple_unfair_lock&&) = delete;
	
	void lock()
	{
		os_unfair_lock_lock(&_lock);
	}
	
	bool trylock()
	{
		return os_unfair_lock_trylock(&_lock);
	}
	
	void unlock()
	{
		os_unfair_lock_unlock(&_lock);
	}
	
private:
	os_unfair_lock _lock;
};

using safe_callbacks_mutex_t = apple_unfair_lock;
using safe_callbacks_recursive_mutex_t = std::recursive_mutex;
}

#else
using safe_callbacks_mutex_t = std::mutex;
using safe_callbacks_recursive_mutex_t = std::recursive_mutex;
#endif

namespace {
template <typename R>
using default_value_t = std::conditional_t<!std::is_void_v<R>, R, std::monostate>;

class safe_callbacks_impl
{
public:
	safe_callbacks_impl(): lock(), cancellables() {}
	safe_callbacks_impl(const safe_callbacks_impl&) = delete;
	safe_callbacks_impl(safe_callbacks_impl&&) = delete;
	~safe_callbacks_impl()
	{
#if SAFE_CALLBACKS_DEBUG_PRINTS
		std::println("Destructing safe_callbacks_impl");
#endif
		std::scoped_lock lock(this->lock);
		for (auto& [key, weak_cancellable] : cancellables)
		{
			if (auto cancellable = weak_cancellable.lock())
			{
				(*cancellable)();
			}
		}
		cancellables.clear();
	}
	
	inline
	void add_cancellable(std::shared_ptr<std::function<void(void)>> cancellable)
	{
#if SAFE_CALLBACKS_DEBUG_PRINTS
		std::println("Adding cancellable");
#endif
		std::scoped_lock lock(this->lock);
		cancellables.insert_or_assign(cancellable.get(), cancellable);
	}
	
	inline
	void remove_cancellable(std::function<void(void)>* cancellablePtr)
	{
#if SAFE_CALLBACKS_DEBUG_PRINTS
		std::println("Removing cancellable");
#endif
		std::scoped_lock lock(this->lock);
		cancellables.erase(cancellablePtr);
	}
	
	safe_callbacks_mutex_t lock;
	std::unordered_map<void*, std::weak_ptr<std::function<void(void)>>> cancellables;
};

template<typename R, typename ...Args>
class safe_function_wrapper;

template<typename R, typename ...Args>
class safe_function_wrapper_impl: public std::enable_shared_from_this<safe_function_wrapper_impl<R, Args...>>
{
public:
	safe_function_wrapper_impl(std::function<R(Args...)>&& callable, default_value_t<R>&& default_value, std::weak_ptr<safe_callbacks_impl> owner): std::enable_shared_from_this<safe_function_wrapper_impl>(), lock(), callable(std::unique_ptr<std::function<R(Args...)>>(new std::function<R(Args...)>{std::forward<std::function<R(Args...)>>(callable)})), default_value(default_value), owner(owner) {}
	safe_function_wrapper_impl() = delete;
	safe_function_wrapper_impl(const safe_function_wrapper_impl&) = delete;
	safe_function_wrapper_impl(safe_function_wrapper_impl&&) = delete;
	~safe_function_wrapper_impl()
	{
#if SAFE_CALLBACKS_DEBUG_PRINTS
		std::println("Destructing safe_function_wrapper_impl");
#endif
		callable.reset();
		remove_cancel();
		cancel.reset();
		owner.reset();
	}
	
	default_value_t<R> default_value;

	inline
	void make_cancel()
	{
		auto func = new std::function<void(void)>(std::bind(&safe_function_wrapper<R, Args...>::cancel, this->weak_from_this()));
		cancel = std::shared_ptr<std::function<void(void)>>(func);
	}
	
	inline
	void remove_cancel()
	{
		if (auto _owner = owner.lock())
		{
			_owner->remove_cancellable(cancel.get());
		}
	}
	
	safe_callbacks_recursive_mutex_t lock;
	std::unique_ptr<std::function<R(Args...)>> callable;
	std::weak_ptr<safe_callbacks_impl> owner;
	std::shared_ptr<std::function<void(void)>> cancel;
};

template<typename R, typename ...Args>
class safe_function_wrapper
{
public:
	safe_function_wrapper(std::function<R(Args...)>&& callable, default_value_t<R>&& default_value, std::weak_ptr<safe_callbacks_impl> owner)
	{
		impl = std::make_shared<safe_function_wrapper_impl<R, Args...>>(std::forward<std::function<R(Args...)>>(callable), std::forward<decltype(default_value)>(default_value), owner);
		impl->make_cancel();
		owner.lock()->add_cancellable(impl->cancel);
	}
	safe_function_wrapper(const safe_function_wrapper&) = default;
	safe_function_wrapper& operator=(const safe_function_wrapper&) = default;
	safe_function_wrapper(safe_function_wrapper&&) = default;
	safe_function_wrapper& operator=(safe_function_wrapper&&) = default;
	~safe_function_wrapper() = default;
	
	inline
	R operator()(Args&&... args) const
	{
		std::function<R(Args...)>* target;
		
		std::scoped_lock lock(impl->lock);
		target = impl->callable.get();
		
		if(target == nullptr)
		{
			auto default_value = impl->default_value;
#if SAFE_CALLBACKS_DEBUG_PRINTS
			std::println("(): ignoring");
#endif
			if constexpr ( !std::is_void_v<R> )
			{
				return default_value;
			}
			else
			{
				return;
			}
		}
		
#if SAFE_CALLBACKS_DEBUG_PRINTS
		std::println("(): executing");
#endif
		return (*target)(std::forward<Args>(args)...);
	}
	
	static void cancel(std::weak_ptr<safe_function_wrapper_impl<R, Args...>> weak_impl)
	{
#if SAFE_CALLBACKS_DEBUG_PRINTS
		std::println("Cancelling function wrapper");
#endif
		if (auto impl = weak_impl.lock())
		{
			std::scoped_lock lock(impl->lock);
			impl->callable.reset();
		}
	}
private:
	std::shared_ptr<safe_function_wrapper_impl<R, Args...>> impl;
};
}

class safe_callbacks
{
public:
	safe_callbacks(): impl(std::make_shared<safe_callbacks_impl>()) {}
	safe_callbacks(const safe_callbacks&): impl(std::make_shared<safe_callbacks_impl>()) {}
	safe_callbacks& operator=(const safe_callbacks&) { return *this; }
	safe_callbacks(safe_callbacks&&): impl(std::make_shared<safe_callbacks_impl>()) {}
	safe_callbacks& operator=(safe_callbacks&&) { return *this; }
	~safe_callbacks() = default;
	
	template <typename C, typename R = std::invoke_result_t<C>, typename = std::enable_if_t< std::is_void_v<R> || std::is_default_constructible_v<R> > > inline
	auto make_safe(C&& callable)
	{
		return make_safe(std::function{std::forward<C>(callable)});
	}
	
	template <typename R, typename ...Args, typename = std::enable_if_t< std::is_void_v<R> || std::is_default_constructible_v<R> > > inline
	std::function<R(Args...)> make_safe(std::function<R(Args...)>&& callable)
	{
		default_value_t<R> dv = {};
		return safe_function_wrapper<R, Args...>(std::forward<std::function<R(Args...)>>(callable), std::forward<decltype(dv)>(dv), impl);
	}
	
	template <typename R, typename C> inline
	auto make_safe(R&& default_value, C&& callable)
	{
		return make_safe(std::forward<R>(default_value), std::function{std::forward<C>(callable)});
	}
	
	template <typename RR, typename R, typename ...Args, typename = std::enable_if_t<!std::is_void_v<R>>> inline
	std::function<R(Args...)> make_safe(RR&& default_value, std::function<R(Args...)> callable)
	{
		return safe_function_wrapper<R, Args...>(std::forward<std::function<R(Args...)>>(callable), std::forward<RR>(default_value), impl);
	}
	
private:
	std::shared_ptr<safe_callbacks_impl> impl;
};
