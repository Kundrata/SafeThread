#pragma once
#define NOMINMAX
#include <windows.h>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <unordered_set>
#include <string>
#include <algorithm>


template <class Duration, class Rep, class Period>
Duration
checked_convert(std::chrono::duration<Rep, Period> d)
{
	using namespace std::chrono;
	using S = duration<double, typename Duration::period>;
	constexpr S m = Duration::min();
	constexpr S M = Duration::max();
	S s = d;
	if (s < m || s > M)
		throw std::overflow_error("checked_convert");
	return duration_cast<Duration>(s);
}

class SingleEvent;

class BinderEvent
{
	// use this clock so that all value of duration are accepted (lowest is nano, only used by the high res clock)
	using clock = std::chrono::high_resolution_clock; 

	std::mutex mtx;
	std::mutex boundEv_mtx;
	std::condition_variable cv;
	std::atomic<bool> event_is_set{ false };
	std::atomic<SingleEvent*> event_source{ nullptr };

	clock::duration max_wait()
	{

	}

public:
	void wait(SingleEvent** ev_source = nullptr) {
		std::unique_lock<std::mutex> lock(mtx);
		cv.wait(lock, [this]() { return (true == event_is_set.load(std::memory_order_acquire)); });
		*ev_source = event_source.load(std::memory_order_acquire);
	};

	bool wait_for(clock::duration t, SingleEvent** ev_source = nullptr) {
		auto t_start = clock::now();
		auto wait_time_no_overflow = [&]() {
			auto t_max = clock::time_point::max();
			auto t_now = clock::now();
			auto d_elapsed = (t_now - t_start);
			auto d_wait = (t > d_elapsed) ? (t - d_elapsed) : clock::duration(0);
			while ((t_max - d_wait).time_since_epoch().count() <= t_now.time_since_epoch().count()) {
				d_wait /= 2;
			}
			return d_wait;
		};

		std::unique_lock<std::mutex> lock(mtx);
		bool pred = false;
		clock::duration d_wait;
		while (pred == false && (d_wait = wait_time_no_overflow()).count() > 0)
			pred = cv.wait_for(lock, d_wait, [this]() {	return (true == event_is_set.load(std::memory_order_acquire)); });

		if (true == pred)
			*ev_source = event_source.load(std::memory_order_acquire);
		else
			*ev_source = nullptr;
		return pred;
	};

	void set(SingleEvent* source)
	{
		event_source.store(source, std::memory_order_release);
		event_is_set.store(true, std::memory_order_release);
		cv.notify_all();
	};
};

class SingleEvent
{
protected:
	// use this clock so that all value of duration are accepted (lowest is nano, only used by the high res clock)
	using clock = std::chrono::high_resolution_clock;

	std::mutex mtx;
	std::mutex boundEv_mtx;
	std::condition_variable cv;
	std::atomic<bool> event_is_set{ false };

	std::unordered_set<BinderEvent*> bound_events;

	void bind_events(BinderEvent* ev)
	{
		std::unique_lock<std::mutex> lock(boundEv_mtx);
		bound_events.insert(ev);
	};

	void unbind_events(BinderEvent* ev)
	{
		std::unique_lock<std::mutex> lock(boundEv_mtx);
		bound_events.erase(ev);
	};

public:

	virtual void wait() {
		std::unique_lock<std::mutex> lock(mtx);
		cv.wait(lock, [this]() { return (true == event_is_set.load(std::memory_order_acquire)); });
	};

	virtual bool wait_for(clock::duration t) {
		auto t_start = clock::now();
		auto wait_time_no_overflow = [&]() {
			auto t_max = clock::time_point::max();
			auto t_now = clock::now();
			auto d_elapsed = (t_now - t_start);
			auto d_wait = (t > d_elapsed) ? (t - d_elapsed) : clock::duration(0);
			while ((t_max - d_wait).time_since_epoch().count() <= t_now.time_since_epoch().count()) {
				d_wait /= 2;
			}
			return d_wait;
		};

		std::unique_lock<std::mutex> lock(mtx);
		bool pred = false;
		clock::duration d_wait;
		while (pred == false && (d_wait = wait_time_no_overflow()).count() > 0)
			pred = cv.wait_for(lock, d_wait, [this]() {	return (true == event_is_set.load(std::memory_order_acquire)); });

		return pred;
	};

	void set()
	{
		{
			//mutex here is ok, because bound events will not have other bindings in turn -- no reciprocal binding can occur, thus no dead lock
			std::unique_lock<std::mutex> lock(boundEv_mtx);
			for (auto ev : bound_events)
				ev->set(this);
		}

		event_is_set.store(true, std::memory_order_release);
		cv.notify_all();
	};

	virtual bool is_set()
	{
		return (true == event_is_set.load(std::memory_order_acquire));
	};

	virtual void reset()
	{
		// SingleEvent is not resettable
	}

	static SingleEvent* wait_multiple_events(std::initializer_list<SingleEvent*> events)
	{
		BinderEvent shared_ev;
		SingleEvent* ev_source;
		for (auto ev : events) {
			if (ev->is_set()) {
				shared_ev.set(ev);
				ev_source = ev;
				break;
			}
			ev->bind_events(&shared_ev);
		}

		shared_ev.wait(&ev_source);
		ev_source->reset();

		for (auto ev : events)
			ev->unbind_events(&shared_ev);

		return ev_source;
	};

	static SingleEvent* wait_multiple_events(std::initializer_list<SingleEvent*> events, clock::duration t)
	{
		BinderEvent shared_ev;
		SingleEvent* ev_source;
		for (auto ev : events) {
			if (ev->is_set()) {
				shared_ev.set(ev);
				ev_source = ev;
				break;
			}
			ev->bind_events(&shared_ev);
		}

		shared_ev.wait_for(t, &ev_source);
		if (nullptr != ev_source)
			ev_source->reset();

		for (auto ev : events)
			ev->unbind_events(&shared_ev);

		return ev_source;
	};
};

class Event : public SingleEvent
{
public:
	void wait() override {
		SingleEvent::wait();
		event_is_set.store(false, std::memory_order_release);
	};

	bool wait_for(clock::duration t) override {
		bool pred = SingleEvent::wait_for(t);
		event_is_set.store(false, std::memory_order_release);
		return pred;
	};

	bool is_set() override
	{
		bool is = (true == event_is_set.load(std::memory_order_acquire));
		event_is_set.store(false, std::memory_order_release);
		return is;
	};

	void reset() override
	{
		event_is_set.store(false, std::memory_order_release);
	}
};





// wrapper for a windows event
class SingleWinEvent
{
protected:
	// use this clock so that all value of duration are accepted (lowest is nano, only used by the high res clock)
	using clock = std::chrono::high_resolution_clock;

	HANDLE hEvent{ NULL };

	void printError() {
		DWORD error = ::GetLastError();
		LPVOID lpMsgBuf;
		FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&lpMsgBuf, 0, NULL);
		std::string strerr = "Error code: " + std::to_string(error) + ", msg: " + std::string((LPCSTR)lpMsgBuf);
		LocalFree(lpMsgBuf);
		OutputDebugStringA(("Failed to create event. Error message: " + strerr).c_str());
	}
public:
	SingleWinEvent() {
		hEvent = CreateEvent(NULL, true, false, NULL);
		if (NULL == hEvent)
			printError();
	}
	~SingleWinEvent() {
		CloseHandle(hEvent);
	}

	// Check if the event was created successfully
	operator bool() const {
		return (NULL != hEvent);
	}
	HANDLE get_handle() {
		return hEvent;
	}

	bool wait() {
		if (WAIT_FAILED == WaitForSingleObject(hEvent, INFINITE))
			return false;
		return true;
	}
	bool wait_for(clock::duration timeout) {
		using _clock = std::chrono::high_resolution_clock;
		std::chrono::time_point<_clock> start = start = _clock::now();
		DWORD res = WaitForSingleObject(hEvent, checked_convert<std::chrono::milliseconds>(timeout).count());
		if (WAIT_FAILED == res) {
			auto time_expired = _clock::now() - start;
			if (time_expired < timeout)
				std::this_thread::sleep_for(timeout - time_expired);
			return false;
		}
		else if (WAIT_TIMEOUT == res)
			return false;
		else
			return true;
	}

	bool set() {
		return SetEvent(hEvent);
	}

	bool is_set() {
		DWORD res = WaitForSingleObject(hEvent, 0);
		if (WAIT_FAILED == res || WAIT_TIMEOUT == res)
			return false;
		else
			return true;
	}

	bool reset() {
		return ResetEvent(hEvent);
	}

	static SingleWinEvent* wait_multiple_events(std::initializer_list<SingleWinEvent*> events) {

		std::vector<HANDLE> handles;
		handles.reserve(events.size());
		std::for_each(events.begin(), events.end(), [&](auto ev) { handles.push_back(ev->hEvent); });

		DWORD idx = WaitForMultipleObjects(handles.size(), &handles[0], FALSE, INFINITE);
		if (WAIT_FAILED == idx)
			return nullptr;
		idx -= WAIT_OBJECT_0;
		return *(events.begin() + idx);
	}

	static SingleWinEvent* wait_multiple_events(std::initializer_list<SingleWinEvent*> events, clock::duration timeout) {
		using _clock = std::chrono::high_resolution_clock;
		std::vector<HANDLE> handles;
		handles.reserve(events.size());
		std::for_each(events.begin(), events.end(), [&](auto ev) { handles.push_back(ev->hEvent); });

		std::chrono::time_point<_clock> start = start = _clock::now();
		DWORD idx = WaitForMultipleObjects(handles.size(), &handles[0], FALSE, checked_convert<std::chrono::milliseconds>(timeout).count());

		// WAIT_FAILED: Sleep until 'timeout' condition is met
		if (WAIT_FAILED == idx) {
			auto time_expired = _clock::now() - start;
			if (time_expired < timeout)
				std::this_thread::sleep_for(timeout - time_expired);
			return nullptr;
		}
		else if (WAIT_TIMEOUT == idx)
			return nullptr;

		idx -= WAIT_OBJECT_0;
		return *(events.begin() + idx);
	}
};

// Like SingleWinEvent, but the event is automatically reset on wait
class WinEvent : public SingleWinEvent
{
public:
	WinEvent() {
		hEvent = CreateEvent(NULL, false, false, NULL);
		if (NULL == hEvent)
			printError();
	}
};