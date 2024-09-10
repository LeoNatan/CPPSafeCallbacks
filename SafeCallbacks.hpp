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
#include <atomic>

#if __cplusplus < 202302L
#undef SAFE_CALLBACKS_DEBUG_PRINTS
#define SAFE_CALLBACKS_DEBUG_PRINTS 0
#else
#ifndef SAFE_CALLBACKS_DEBUG_PRINTS
#define SAFE_CALLBACKS_DEBUG_PRINTS 0
#else
#if DEBUG
#define SAFE_CALLBACKS_GET_NAME(impl) impl->name.length() > 0 ? impl->name : "<unnamed>"
#else
#define SAFE_CALLBACKS_GET_NAME(impl) "<unnamed>"
#endif
#endif
#endif

#if SAFE_CALLBACKS_DEBUG_PRINTS
#include <print>
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
		
	}
	
	inline
	void add_cancellable(std::shared_ptr<std::function<void(void)>> cancellable)
	{
		if(is_cancelled)
		{
			return;
		}
		
#if SAFE_CALLBACKS_DEBUG_PRINTS
		std::println("Adding cancellable");
#endif
		std::lock_guard lock(this->lock);
		cancellables.insert_or_assign(cancellable.get(), cancellable);
	}
	
	inline
	void remove_cancellable(std::function<void(void)>* cancellablePtr)
	{
		if(is_cancelled)
		{
			return;
		}
		
#if SAFE_CALLBACKS_DEBUG_PRINTS
		std::println("Removing cancellable");
#endif
		std::lock_guard lock(this->lock);
		cancellables.erase(cancellablePtr);
	}
	
	std::atomic<bool> is_cancelled = false;
	std::mutex lock;
	std::unordered_map<void*, std::weak_ptr<std::function<void(void)>>> cancellables;
};

template<typename R, typename ...Args>
class safe_function_wrapper;

template<typename R, typename ...Args>
class safe_function_wrapper_impl: public std::enable_shared_from_this<safe_function_wrapper_impl<R, Args...>>
{
public:
	safe_function_wrapper_impl(std::function<R(Args...)>&& callable,
							   default_value_t<R>&& default_value,
							   std::weak_ptr<safe_callbacks_impl> owner,
							   const char* &&name):
	std::enable_shared_from_this<safe_function_wrapper_impl>(), lock(), callable(std::unique_ptr<std::function<R(Args...)>>(new std::function<R(Args...)>{std::forward<std::function<R(Args...)>>(callable)})), default_value(default_value), owner(owner)
#if DEBUG
	, name(name)
#endif
	{}
	
	safe_function_wrapper_impl() = delete;
	safe_function_wrapper_impl(const safe_function_wrapper_impl&) = delete;
	safe_function_wrapper_impl(safe_function_wrapper_impl&&) = delete;
	~safe_function_wrapper_impl()
	{
#if SAFE_CALLBACKS_DEBUG_PRINTS
		std::println("Destructing safe_function_wrapper_impl of {0}", SAFE_CALLBACKS_GET_NAME(this));
#endif
		callable.reset();
		remove_cancel();
		cancel.reset();
		owner.reset();
#if DEBUG
		name.clear();
#endif
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
	
	std::recursive_mutex lock;
	std::unique_ptr<std::function<R(Args...)>> callable;
	std::weak_ptr<safe_callbacks_impl> owner;
	std::shared_ptr<std::function<void(void)>> cancel;
#if DEBUG
	std::string name;
#endif
};

template<typename R, typename ...Args>
class safe_function_wrapper
{
public:
	safe_function_wrapper(std::function<R(Args...)>&& callable, default_value_t<R>&& default_value, std::weak_ptr<safe_callbacks_impl> owner, const char* &&name)
	{
		auto locked = owner.lock();
		impl = std::make_shared<safe_function_wrapper_impl<R, Args...>>(std::forward<std::function<R(Args...)>>(callable), std::forward<decltype(default_value)>(default_value), owner, std::forward<const char*>(name));
		if(locked->is_cancelled)
		{
			safe_function_wrapper::cancel(impl);
		}
		else
		{
			impl->make_cancel();
			locked->add_cancellable(impl->cancel);
		}
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
		
		std::lock_guard lock(impl->lock);
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
		if (auto impl = weak_impl.lock())
		{
#if SAFE_CALLBACKS_DEBUG_PRINTS
			std::println("Cancelling function wrapper of {0}", SAFE_CALLBACKS_GET_NAME(impl));
#endif
			std::lock_guard lock(impl->lock);
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
	~safe_callbacks()
	{
#if SAFE_CALLBACKS_DEBUG_PRINTS
		std::println("Destructing safe_callbacks");
#endif
		impl->is_cancelled = true;
		
		std::lock_guard lock(impl->lock);
		for (auto& [key, weak_cancellable] : impl->cancellables)
		{
			if (auto cancellable = weak_cancellable.lock())
			{
				(*cancellable)();
			}
		}
		impl->cancellables.clear();
	}
	
	template <typename C> inline
	auto make_safe(C&& callable, const char*&& name = "")
	{
		return make_safe(std::function{std::forward<C>(callable)}, std::forward<const char*>(name));
	}

	template <typename R, typename ...Args> inline
	std::function<R(Args...)> make_safe(std::function<R(Args...)>&& callable, const char*&& name = "")
	{
		default_value_t<R> dv = {};
		return safe_function_wrapper<R, Args...>(std::forward<std::function<R(Args...)>>(callable), std::forward<decltype(dv)>(dv), impl, std::forward<const char*>(name));
	}

	template <typename R, typename C> inline
	auto make_safe(R&& default_value, C&& callable, const char*&& name = "")
	{
		return make_safe(std::forward<R>(default_value), std::function{std::forward<C>(callable)}, std::forward<const char*>(name));
	}
	
	template <typename RR, typename R, typename ...Args> inline
	std::function<R(Args...)> make_safe(RR&& default_value, std::function<R(Args...)> callable, const char*&& name = "")
	{
		return safe_function_wrapper<R, Args...>(std::forward<std::function<R(Args...)>>(callable), std::forward<RR>(default_value), impl, std::forward<const char*>(name));
	}
	
private:
	std::shared_ptr<safe_callbacks_impl> impl;
};
