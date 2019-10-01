#pragma once
#define NOMINMAX
#include <windows.h>
#include "Event.h"
#include "TypeTraits.h"
#include "logger.h"
#include "NamedType.h"
#include "StackWalker.h"
#include <thread>
#include <type_traits>
#include <string>
#include <sstream>
#include <mutex>
#include <unordered_set>
#include <atomic>





template <typename T>
class ref_wrapper {

	T* _ptr;
public:

	ref_wrapper() noexcept : _ptr(nullptr) {}
	ref_wrapper(T& ref) noexcept : _ptr(std::addressof(ref)) {}
	ref_wrapper(T&&) = delete;
	ref_wrapper(const ref_wrapper&) noexcept = default;

	ref_wrapper& operator=(const ref_wrapper& x) noexcept = default;

	operator T& () const noexcept { return *_ptr; }
	T& get() const noexcept { return *_ptr; }
};

template<typename T>
class atomic_ref {

	std::atomic<ref_wrapper<T>> atref;
public:

	atomic_ref(T& ref) noexcept {
		atref.store(ref, std::memory_order_release);
	}

	atomic_ref<T>& operator=(const atomic_ref<T>& x) noexcept {
		atref.store(x.get(), std::memory_order_release);
		return *this;
	};

	operator T& () const noexcept { return atref.load(std::memory_order_acquire).get(); }
	T& get() const noexcept { return atref.load(std::memory_order_acquire).get(); }
};



class tracked_exception : public std::exception {
protected:
	EXCEPTION_POINTERS* pExp{ nullptr };
	tracked_exception(EXCEPTION_POINTERS* pe = nullptr) : pExp(pe) {}
	tracked_exception(const tracked_exception& rhs) : pExp(rhs.pExp) {}
public:
	EXCEPTION_POINTERS* getExceptionPointers() { return pExp; }
};

class ccW_exception : public tracked_exception {
	std::exception original_cc_exc;

public:
	ccW_exception(std::exception&& original, EXCEPTION_POINTERS* pe = nullptr) : tracked_exception(pe), original_cc_exc(std::move(original)) {}
	ccW_exception(const ccW_exception& rhs) : tracked_exception(rhs), original_cc_exc(std::move(rhs.original_cc_exc)) {}

	const char* what() const override {
		return original_cc_exc.what();
	}
};

class SE_exception : public tracked_exception {
private:
	SE_exception() {}
	unsigned int nSE;
	mutable std::string s_what{ "" };
public:
	SE_exception(unsigned int n, EXCEPTION_POINTERS* pe = nullptr) : tracked_exception(pe), nSE(n) {}
	SE_exception(const SE_exception& rhs) : tracked_exception(rhs), nSE(rhs.nSE) {}
	~SE_exception() {}
	unsigned int getSeNumber() { return nSE; }
	const char* what() const override {
		if (s_what.empty()) {
			std::stringstream ss;
			ss.flags(std::ios::hex);
			ss << "Structured exception, code: " << nSE;
			s_what = ss.str();
		}
		return s_what.c_str();
	}
};



namespace Threading {

	class SafeThread
	{
	protected:

		class SharedInst {
			mutable std::mutex s_mtx;
			mutable std::unordered_set<SafeThread*> active_threads;
		public:
			static SharedInst& inst() {
				static SharedInst i;
				return i;
			}
			~SharedInst() {	}

			void add_thread(SafeThread* t) const {
				std::unique_lock<std::mutex> lock(s_mtx);
				active_threads.insert(t);
			}
			void remove_thread(SafeThread* t) const {
				std::unique_lock<std::mutex> lock(s_mtx);
				if (active_threads.empty())
					return;
				active_threads.erase(t);
			}
			template<typename F>
			void active_threads_map(F apply) const
			{
				std::unique_lock<std::mutex> lock(s_mtx);
				for (auto &t : active_threads)
				{
					lock.unlock(); // unlock while calling external function

					apply(t);

					lock.lock();
				}
			}
		};


		using ExHnd = std::function<bool(SafeThread&, tracked_exception&)>;

	public:

		using ExceptionHandler = NamedType<ExHnd, struct ExceptionHandlerTag>;
		using Frozen = NamedType<bool, struct FrozenTag>;


	private:

		static std::wstring s2ws(const std::string& s) {
			int len;
			int slength = (int)s.length();
			len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), slength, 0, 0);
			std::wstring r(len, L'\0');
			MultiByteToWideChar(CP_ACP, 0, s.c_str(), slength, &r[0], len);
			return r;
		}
		static const std::wstring& s2ws(const std::wstring& s) {
			return s;
		}


	protected:
		std::wstring name{ L"unnamed" };
		ExHnd exception_handler{ defaultExHandler };
		std::mutex name_mtx;
		std::mutex ex_mtx;
		std::thread thread;
		std::atomic<SingleEvent*> unfreeze_event{ nullptr };
		std::unique_ptr<atomic_ref<SafeThread>> owner;
		static const inline std::unique_ptr<SharedInst> shared{ std::make_unique<SharedInst>() };



	private:
		void move_thread(SafeThread&& t) {
			thread = std::move(t.thread);
			setName(std::move(t.name));
			setExceptionHandler(ExceptionHandler(t.exception_handler));
			owner = std::move(t.owner);
			*owner = *this;
			auto ev = t.unfreeze_event.load(std::memory_order_acquire);
			unfreeze_event.store(ev, std::memory_order_release);
			shared->remove_thread(&t);
			shared->add_thread(this);
		}

	public:
		static bool defaultExHandler(SafeThread& t, tracked_exception& ex)
		{
			std::wstringstream wss;
			wss.flags(std::ios::hex);
			wss << L"Thread \"" << t.name << "\" -> (hnd: " << t.native_handle() << ", id: " <<
				GetThreadId(t.native_handle()) << ") encountered exception " << s2ws(ex.what()) << std::endl;

			Stackwalk::StackWalker::passPrettyTrace([&](const std::string& trce) {
				wss << L"Stack trace: " << std::endl << s2ws(trce) << std::endl;
			}, ex.getExceptionPointers()->ContextRecord);

			OutputDebugStringW(wss.str().c_str());
			fwprintf(stderr, wss.str().c_str());
			Logger::defprintf(wss.str());

			return false;
		}

	private:

		template<typename Func, typename Handler>
		static void try_catch_wrapper(Func& f, Handler& h)
		{

			std::exception* original = new std::exception;

			auto handle_seh = [&](unsigned int code, EXCEPTION_POINTERS* pExp) {

				if (code == 0xE06D7363) {
					ccW_exception ex(std::move(*original), pExp);
					h(ex);
				}
				else {
					SE_exception ex(code, pExp);
					h(ex);
				}
				return EXCEPTION_EXECUTE_HANDLER;
			};

			__try {
				[&]() {
					try {
						f();
					}
					catch (std::exception& ex) {
						*original = ex;
						throw;
					}
				}();
			}
			__except (handle_seh(GetExceptionCode(), GetExceptionInformation())) {}

			delete original;
		};


		template<typename F, typename... Args,
			typename = std::enable_if<_is_invocable<F, Args...>::value>::type>
		void WrapAndLaunch(F&& f, Args&&... args)
		{
			owner = std::make_unique<atomic_ref<SafeThread>>(*this);

			SingleEvent* p_unfreeze_ev = unfreeze_event.load(std::memory_order_relaxed);

			auto wrapped = [owner = this->owner.get(), p_unfreeze_ev](auto&& func, auto&&... arguments) mutable {

				if (p_unfreeze_ev) {
					p_unfreeze_ev->wait();
					delete p_unfreeze_ev;
				}

				bool reenter = false;

				do
				{
					try_catch_wrapper(
						[&]() {
							std::invoke(func, std::forward<decltype(arguments)>(arguments)...);
						},
						[&](tracked_exception& ex) {
							// get a temporary copy of the function and call it
							// to avoid a deadlock due to holding the lock while calling an external function
							std::unique_lock<std::mutex> lock(owner->get().ex_mtx);
							auto temp = owner->get().exception_handler;
							lock.unlock();
							reenter = temp(owner->get(), ex);
						});

				} while (reenter);

			};
			thread = std::thread(std::move(wrapped), std::forward<F>(f), std::forward<Args>(args)...);
			shared->add_thread(this);
		}

		template<typename Str,
			typename = std::enable_if<is_string<Str>::type::value, is_string<Str>>::type,
			typename... Args>
		void WrapAndLaunch(Str&& _name, Args&&... args)
		{
			setName(std::forward<Str>(_name));
			WrapAndLaunch(std::forward<Args>(args)...);
		}

		template<typename... Args>
		void WrapAndLaunch(const ExceptionHandler& exh, Args&&... args)
		{
			setExceptionHandler(exh);
			WrapAndLaunch(std::forward<Args>(args)...);
		}

		template<typename... Args>
		void WrapAndLaunch(ExceptionHandler&& exh, Args&&... args)
		{
			setExceptionHandler(std::move(exh));
			WrapAndLaunch(std::forward<Args>(args)...);
		}

		template<typename... Args>
		void WrapAndLaunch(Frozen launch_frozen, Args&&... args)
		{
			if (launch_frozen.get() == true)
				unfreeze_event.store(new SingleEvent, std::memory_order_relaxed);

			WrapAndLaunch(std::forward<Args>(args)...);

			if (launch_frozen.get() == false)
				unfreeze();
		}

	public:

		SafeThread() {}

		template<typename... Args>
		SafeThread(Args&&... args)
		{
			WrapAndLaunch(std::forward<Args>(args)...);
		}

		SafeThread(SafeThread&& t) {
			move_thread(std::move(t));
		};

		SafeThread& operator=(SafeThread&& t) {
			move_thread(std::move(t));
			return *this;
		}

		~SafeThread() {
			if (thread.joinable())
				thread.join();
			shared->remove_thread(this);
		}



		template<typename Str,
			typename = std::enable_if<is_string<Str>::type::value>::type>
		void setName(Str&& _name) {
			std::unique_lock<std::mutex> lock(name_mtx);
			name = s2ws(std::forward<Str>(_name));
		}

		void setExceptionHandler(const ExceptionHandler& exh) {
			std::unique_lock<std::mutex> lock(ex_mtx);
			exception_handler = exh.get();
		}
		void setExceptionHandler(ExceptionHandler&& exh) {
			std::unique_lock<std::mutex> lock(ex_mtx);
			exception_handler = std::move(exh.get());
		}

		void unfreeze() {
			SingleEvent* ev;
			if ((ev = unfreeze_event.load(std::memory_order_acquire)) != nullptr) {
				// the frozen thread is responsible for deleting the event object
				ev->set();
				unfreeze_event.store(nullptr, std::memory_order_release);
			}
		}

		void* native_handle() {
			return thread.native_handle();
		}
		bool joinable() {
			return thread.joinable();
		}
		void join() {
			thread.join();
			shared->remove_thread(this);
		}

		template<typename F>
		static void active_threads_map(F apply) {
			shared->active_threads_map(apply);
		}

	};
}

