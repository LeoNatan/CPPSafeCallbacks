//
//  SafeCallbacks.hpp
//  SafeCallbacks
//
//  Created by Léo Natan on 05/09/2024.
//

#include <os/lock.h>

#include <memory>
#include <variant>
#include <unordered_map>
#include <functional>

#ifndef SAFE_CALLBACKS_DEBUG_PRINTS
#define SAFE_CALLBACKS_DEBUG_PRINTS 0
#endif

#if SAFE_CALLBACKS_DEBUG_PRINTS
#include <print>
#endif

class safe_callbacks
{
public:
	template <typename R>
	using default_value_t = std::conditional_t<!std::is_void_v<R>, R, std::monostate>;
	
	safe_callbacks(): impl(std::make_shared<safe_callbacks_impl>()) {}
	safe_callbacks(const safe_callbacks&): impl(std::make_shared<safe_callbacks_impl>()) {}
	safe_callbacks(safe_callbacks&&): impl(std::make_shared<safe_callbacks_impl>()) {}
	~safe_callbacks()
	{
#if SAFE_CALLBACKS_DEBUG_PRINTS
		std::println("Destructing safe_callbacks");
#endif
		os_unfair_lock_lock(&impl->lock);
		for (auto& [key, weak_cancellable] : impl->cancellables)
		{
			if (auto cancellable = weak_cancellable.lock())
			{
				(*cancellable)();
			}
		}
		impl->cancellables.clear();
		os_unfair_lock_unlock(&impl->lock);
	}
	
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
	class safe_callbacks_impl
	{
	public:
		safe_callbacks_impl(): lock(OS_UNFAIR_LOCK_INIT), cancellables() {}
		safe_callbacks_impl(const safe_callbacks_impl&) = delete;
		safe_callbacks_impl(safe_callbacks_impl&&) = delete;
		
		friend class safe_callbacks;
		
	private:
		inline
		void add_cancellable(std::shared_ptr<std::function<void(void)>> cancellable)
		{
#if SAFE_CALLBACKS_DEBUG_PRINTS
			std::println("Adding cancellable");
#endif
			os_unfair_lock_lock(&lock);
			cancellables.insert_or_assign(cancellable.get(), cancellable);
			os_unfair_lock_unlock(&lock);
		}
		
		inline
		void remove_cancellable(std::function<void(void)>* cancellablePtr)
		{
#if SAFE_CALLBACKS_DEBUG_PRINTS
			std::println("Removing cancellable");
#endif
			os_unfair_lock_lock(&lock);
			cancellables.erase(cancellablePtr);
			os_unfair_lock_unlock(&lock);
		}
		
		os_unfair_lock lock;
		std::unordered_map<void*, std::weak_ptr<std::function<void(void)>>> cancellables;
	};
	
	template<typename R, typename ...Args>
	class safe_function_wrapper
	{
	private:
		class safe_function_wrapper_impl: public std::enable_shared_from_this<safe_function_wrapper_impl>
		{
		public:
			safe_function_wrapper_impl(std::function<R(Args...)>&& callable, default_value_t<R>&& default_value, std::weak_ptr<safe_callbacks_impl> owner): std::enable_shared_from_this<safe_function_wrapper_impl>(), lock(OS_UNFAIR_LOCK_INIT), callable(std::unique_ptr<std::function<R(Args...)>>(new std::function<R(Args...)>{std::forward<std::function<R(Args...)>>(callable)})), default_value(default_value), owner(owner) {}
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
			
			friend class safe_function_wrapper;
			
			default_value_t<R> default_value;
		private:
			inline
			void make_cancel()
			{
				auto func = new std::function<void(void)>(std::bind(&safe_function_wrapper::cancel, this->weak_from_this()));
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
			
			os_unfair_lock lock;
			std::unique_ptr<std::function<R(Args...)>> callable;
			std::weak_ptr<safe_callbacks_impl> owner;
			std::shared_ptr<std::function<void(void)>> cancel;
		};
		
	public:
		safe_function_wrapper(std::function<R(Args...)>&& callable, default_value_t<R>&& default_value, std::weak_ptr<safe_callbacks_impl> owner)
		{
			impl = std::make_shared<safe_function_wrapper_impl>(std::forward<std::function<R(Args...)>>(callable), std::forward<decltype(default_value)>(default_value), owner);
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
			os_unfair_lock_lock(&impl->lock);
			auto target = impl->callable.get();
			if(target == nullptr)
			{
				auto default_value = impl->default_value;
				os_unfair_lock_unlock(&impl->lock);
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
			os_unfair_lock_unlock(&impl->lock);

#if SAFE_CALLBACKS_DEBUG_PRINTS
			std::println("(): executing");
#endif
			return (*target)(std::forward<Args>(args)...);
		}

		static void cancel(std::weak_ptr<safe_function_wrapper_impl> weak_impl)
		{
#if SAFE_CALLBACKS_DEBUG_PRINTS
			std::println("Cancelling function wrapper");
#endif
			if (auto impl = weak_impl.lock())
			{
				os_unfair_lock_lock(&impl->lock);
				impl->callable.reset();
				os_unfair_lock_unlock(&impl->lock);
			}
		}
	private:
		std::shared_ptr<safe_function_wrapper_impl> impl;
	};
	
	std::shared_ptr<safe_callbacks_impl> impl;
};
