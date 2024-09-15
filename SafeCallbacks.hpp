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

#if !(__cplusplus > 202002L)
#undef SAFE_CALLBACKS_DEBUG_PRINTS
#define SAFE_CALLBACKS_DEBUG_PRINTS 0
#elif !defined(SAFE_CALLBACKS_DEBUG_PRINTS)
#define SAFE_CALLBACKS_DEBUG_PRINTS 0
#elif DEBUG
#define SAFE_CALLBACKS_GET_NAME(impl) impl->name.length() > 0 ? impl->name : "<unnamed>"
#else
#define SAFE_CALLBACKS_GET_NAME(impl) "<unnamed>"
#endif

#if SAFE_CALLBACKS_DEBUG_PRINTS
#include <print>
#endif

namespace {
template <typename DVR>
using default_value_t = std::conditional_t<!std::is_void_v<DVR>, DVR, std::monostate>;

template <typename R>
struct is_constructible_rv : std::conditional_t<std::is_void_v<R> || std::is_constructible_v<R>, std::true_type, std::false_type> {};
template <typename R>
static inline constexpr bool is_constructible_rv_v = is_constructible_rv<R>::value;

template <typename DVR, typename R>
struct is_compatible_rv : std::conditional_t<std::is_same_v<DVR, R> || std::is_convertible_v<DVR, R>, std::true_type, std::false_type> {};
template <typename DVR, typename R>
static inline constexpr bool is_compatible_rv_v = is_compatible_rv<DVR, R>::value;

template <typename DVR>
struct is_returnable_rv : std::conditional_t<std::is_copy_constructible_v<DVR> || std::is_move_constructible_v<DVR>, std::true_type, std::false_type> {};
template <typename DVR>
static inline constexpr bool is_returnable_rv_v = is_returnable_rv<DVR>::value;

// Helper class. Instances of this class may be released after the main safe_callbacks instance is released,
// but it will be marked as is_cancelled=true and all registered callables will be cancelled.
class safe_callbacks_impl
{
public:
	safe_callbacks_impl(): lock(), cancellables() {}
	safe_callbacks_impl(const safe_callbacks_impl&) = delete;
	safe_callbacks_impl& operator=(const safe_callbacks_impl&) = delete;
	
	inline
	void add_cancellable(std::shared_ptr<std::function<void(void)>> cancellable)
	{
		if(is_cancelled)
		{
			(*cancellable)();
			return;
		}
		
#if SAFE_CALLBACKS_DEBUG_PRINTS
		std::println("Adding cancellable");
#endif
		std::lock_guard lock(this->lock);
		
		if(is_cancelled)
		{
			(*cancellable)();
			return;
		}
		
		//Adds a std::weak_ptr of the cancellable.
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

template <typename DVR, typename F>
class safe_function_wrapper;

template<typename DVR, typename R, typename ...Args>
class safe_function_wrapper_impl: public std::enable_shared_from_this<safe_function_wrapper_impl<DVR, R, Args...>>
{
public:
	safe_function_wrapper_impl(std::function<R(Args...)>&& callable,
							   default_value_t<DVR>&& default_return_value,
							   std::weak_ptr<safe_callbacks_impl> owner,
							   const char* &&name):
	std::enable_shared_from_this<safe_function_wrapper_impl>(), lock(), callable(std::make_unique<std::function<R(Args...)>>(std::move(callable))), owner(owner), default_return_value(std::forward<default_value_t<DVR>>(default_return_value))
#if DEBUG
	, name(std::move(name))
#endif
	{}
	safe_function_wrapper_impl() = delete;
	safe_function_wrapper_impl(const safe_function_wrapper_impl&) = delete;
	safe_function_wrapper_impl& operator=(const safe_function_wrapper_impl&) = delete;
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
	
	default_value_t<DVR> default_return_value;

	inline
	void make_cancel()
	{
		cancel = std::make_shared<std::function<void(void)>>(std::bind(&safe_function_wrapper<DVR, R(Args...)>::cancel, this->weak_from_this()));
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

template<typename DVR, typename R, typename ...Args>
class safe_function_wrapper<DVR, R(Args...)>
{
public:
	safe_function_wrapper(std::function<R(Args...)>&& callable, default_value_t<DVR>&& default_return_value, std::weak_ptr<safe_callbacks_impl> owner, const char* &&name)
	{
		auto locked = owner.lock();
		impl = std::make_shared<safe_function_wrapper_impl<DVR, R, Args...>>(std::forward<std::function<R(Args...)>>(callable), std::forward<default_value_t<DVR>>(default_return_value), owner, std::forward<const char*>(name));
		impl->make_cancel();
		locked->add_cancellable(impl->cancel);
	}
	
	inline
	R operator()(Args&&... args) const
	{
		std::function<R(Args...)>* target;
		
		std::lock_guard lock(impl->lock);
		target = impl->callable.get();
		
		if(target == nullptr)
		{
#if SAFE_CALLBACKS_DEBUG_PRINTS
			std::println("(): ignoring");
#endif
			if constexpr(std::is_void_v<R>)
			{
				// Original callable had a void return type.
				return;
			}
			else if constexpr(std::is_void_v<DVR>)
			{
				// No default value was provided to make_safe().
				return {};
			}
			else if constexpr(std::is_copy_constructible_v<DVR>)
			{
				// The provided default value is copy constructible, return by value.
				return impl->default_return_value;
			}
			else
			{
				// The provided default value is not copy constructible, return by move.
				// Further calls to the wrapper is undefined behavior.
				return std::move(impl->default_return_value);
			}
			
#if __cplusplus > 202002L
			std::unreachable();
#endif
		}
		
#if SAFE_CALLBACKS_DEBUG_PRINTS
		std::println("(): executing");
#endif
		return (*target)(std::forward<Args>(args)...);
	}
	
	friend class safe_function_wrapper_impl<DVR, R, Args...>;
	
protected:
	static void cancel(std::weak_ptr<safe_function_wrapper_impl<DVR, R, Args...>> weak_impl)
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
	std::shared_ptr<safe_function_wrapper_impl<DVR, R, Args...>> impl;
};
}

class safe_callbacks
{
public:
	safe_callbacks(): impl(std::make_shared<safe_callbacks_impl>()) {}
	// These are explicitly allowed and do nothing on purpose.
	// Wrapped callables are tied to a specific object, and should not be copied or moved.
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
		
	/// Creates a safe `std::function` wrapper around `callable` and ties its lifetime to the owner's.
	///
	/// If the owning object is released, the wrapper function is cancelled and automatically replaces the wrapped `callable` with a no-op.
	///
	/// In case of cancellation, if the return type of `callable` is not void, the wrapper function constructs and returns a default return value.
	/// - Parameter callable: A callable to make safe
	template <typename C> inline
	auto make_safe(C&& callable, const char*&& name = "")
	{
		return make_safe(std::function{std::forward<C>(callable)}, std::forward<const char*>(name));
	}
	
	/// Creates a safe `std::function` wrapper around `callable` and ties its lifetime to the owner's.
	///
	/// If the owning object is released, the wrapper function is cancelled and automatically replaces the wrapped `callable` with a no-op.
	///
	/// In case of cancellation, in cases where the return type of `callable` is not void, the wrapper function constructs and returns a default return value.
	/// - Parameter callable: A `std::function` to make safe
	template <typename R, typename ...Args> inline
	safe_function_wrapper<void, R(Args...)> make_safe(std::function<R(Args...)>&& callable, const char*&& name = "")
	{
		static_assert(is_constructible_rv_v<R>, "Return value type is not constructible");
		return safe_function_wrapper<void, R(Args...)>(std::forward<std::function<R(Args...)>>(callable), {}, impl, std::forward<const char*>(name));
	}

	/// Creates a safe `std::function` wrapper around `callable` and ties its lifetime to the owner's.
	///
	/// If the owning object is released, the wrapper function is cancelled and automatically replaces the wrapped `callable` with a no-op.
	///
	/// In case of cancellation, the wrapper function returns the provided default return value. If the default return value is copy constructible,
	/// it is returned by value. Otherwise, it is returned by move. In case of return by move, it is undefined behavior of the wrapper function
	/// is called more than once.
	/// - Parameter default_return_value: The default return value to return in case the wrapper is cancelled
	/// - Parameter callable: A callable to make safe
	template <typename DVR, typename C> inline
	auto make_safe(DVR&& default_return_value, C&& callable, const char*&& name = "")
	{
		return make_safe(std::forward<DVR>(default_return_value), std::function{std::forward<C>(callable)}, std::forward<const char*>(name));
	}
	
	/// Creates a safe `std::function` wrapper around `callable` and ties its lifetime to the owner's.
	///
	/// If the owning object is released, the wrapper function is cancelled and automatically replaces the wrapped `callable` with a no-op.
	///
	/// In case of cancellation, the wrapper function returns the provided default return value. If the default return value is copy constructible,
	/// it is returned by value. Otherwise, it is returned by move. In case of return by move, it is undefined behavior if the wrapper function
	/// is called more than once.
	/// - Parameter default_return_value: The default return value to return in case the wrapper is cancelled
	/// - Parameter callable: A `std::function` to make safe
	template <typename DVR, typename R, typename ...Args> inline
	safe_function_wrapper<DVR, R(Args...)> make_safe(DVR&& default_return_value, std::function<R(Args...)>&& callable, const char*&& name = "")
	{
		static_assert(is_compatible_rv_v<DVR, R>, "Incompatible default return value type");
		static_assert(is_returnable_rv_v<DVR>, "Unsupported default return value type");
		return safe_function_wrapper<DVR, R(Args...)>(std::forward<std::function<R(Args...)>>(callable), std::forward<DVR>(default_return_value), impl, std::forward<const char*>(name));
	}
	
private:
	std::shared_ptr<safe_callbacks_impl> impl;
};
