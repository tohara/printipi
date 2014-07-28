#ifndef SCHEDULER_H
#define SCHEDULER_H

//#include <queue>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <time.h> //for timespec
#include <chrono> 
#include <array>
#include <vector>
#include <atomic>
#include <tuple>
#include <algorithm> //for push_heap
//#include <functional>
#include "event.h"
#include "logging.h"
#include "timeutil.h"

//#include <pthread.h> //for pthread_setschedparam
//#include <time.h> //for clock_nanosleep
//#include "logging.h"

#ifndef SCHED_PRIORITY
	#define SCHED_PRIORITY 30
#endif
#ifndef SCHED_CAPACITY
	#define SCHED_CAPACITY 512
#endif
#ifndef SCHED_NUM_EXIT_HANDLER_LEVELS
	#define SCHED_NUM_EXIT_HANDLER_LEVELS 2
#endif
#define SCHED_IO_EXIT_LEVEL 0
#define SCHED_MEM_EXIT_LEVEL 1

struct PwmInfo {
	unsigned nsHigh;
	unsigned nsLow;
	PwmInfo() : nsHigh(0), nsLow(0) {}
	PwmInfo(float duty, float period) : 
		nsHigh(std::max(0, (int)(duty*period*1000000000))), //clamp the times to >= 0
		nsLow(std::max(0, (int)((1-duty)*period*1000000000))) {}
	float period() const {
		return nsHigh + nsLow;
	}
	bool isNonNull() const {
		return nsHigh || nsLow;
	}
};

class SchedulerBase {
	static std::array<std::vector<void(*)()>, SCHED_NUM_EXIT_HANDLER_LEVELS> exitHandlers;
	static std::atomic<bool> isExiting; //typical implementations of exit() call the exit handlers from within the thread that called exit. Therefore, if the exiting thread causes another thread to call exit(), this value must be atomic.
	private:
		static void callExitHandlers();
	public:
		static void configureExitHandlers();
		static void registerExitHandler(void (*handler)(), unsigned level);
};

template <typename Interface> class Scheduler : public SchedulerBase {
	Interface interface;
	std::array<PwmInfo, 256> pwmInfo; 
	//std::queue<Event> eventQueue;
	std::deque<Event> eventQueue;
	
	mutable std::mutex mutex;
	//std::unique_lock<std::mutex> _lockPushes;
	//bool _arePushesLocked;
	std::condition_variable nonemptyCond;
	std::condition_variable eventConsumedCond;
	
	//struct timespec lastEventHandledTime; //used? Only written - never read!
	unsigned bufferSize;
	//static std::array<std::vector<void(*)()>, SCHED_NUM_EXIT_HANDLER_LEVELS> exitHandlers;
	//static std::atomic<bool> isExiting; //typical implementations of exit() call the exit handlers from within the thread that called exit. Therefore, if the exiting thread causes another thread to call exit(), this value must be atomic.
	private:
		//static void callExitHandlers();
		void orderedInsert(const Event &evt);
	public:
		//static void configureExitHandlers();
		//static void registerExitHandler(void (*handler)(), unsigned level);
		//queue and nextEvent can be called from separate threads, but nextEvent must NEVER be called from multiple threads.
		void queue(const Event &evt);
		void schedPwm(AxisIdType idx, const PwmInfo &p);
		inline void schedPwm(AxisIdType idx, float duty) {
			PwmInfo pi(duty, pwmInfo[idx].period());
			schedPwm(idx, pi);
		}
		Scheduler(Interface interface);
		Event nextEvent(bool doSleep=true, std::chrono::microseconds timeout=std::chrono::microseconds(1000000));
		void sleepUntilEvent(const Event &evt) const;
		void initSchedThread() const; //call this from whatever threads call nextEvent to optimize that thread's priority.
		struct timespec lastSchedTime() const; //get the time at which the last event is scheduled, or the current time if no events queued.
		void setBufferSize(unsigned size);
		unsigned getBufferSize() const;
		unsigned numActivePwmChannels() const;
		template <typename OnEvent, typename OnWait> void eventLoop(OnEvent onEvent, OnWait onWait) {
			Event evt;
			while (1) {
				bool needCpuTime = onWait();
				evt = this->nextEvent(false, std::chrono::microseconds(needCpuTime ? 0 : 100000)); //get next event, but don't sleep. Yield to the OS for ~100ms if we DON'T need the cpu time.
				if (!evt.isNull()) { //wait for event to occur if it is non-null.
					while (!evt.isTime()) {
						if (!onWait()) { //if we don't need any future waiting, then sleep to give cpu to other processes:
							this->sleepUntilEvent(evt);
						}
					}
					onEvent(evt);
					needCpuTime = true;
				}
			}
		}
};



template <typename Interface> Scheduler<Interface>::Scheduler(Interface interface) 
	: interface(interface),bufferSize(SCHED_CAPACITY) {
	//clock_gettime(CLOCK_MONOTONIC, &(this->lastEventHandledTime)); //initialize to current time.
}


template <typename Interface> void Scheduler<Interface>::queue(const Event& evt) {
	//LOGV("Scheduler::queue\n");
	std::unique_lock<std::mutex> lock(this->mutex);
	while (this->eventQueue.size() >= this->bufferSize) {
		eventConsumedCond.wait(lock);
	}
	//_lockPushes.lock(); //aquire a lock
	//if (this->eventQueue.size() >= this->bufferSize) {
	//	return; //keep pushes locked.
	this->orderedInsert(evt);
	this->nonemptyCond.notify_one(); //notify the consumer thread that a new event is ready.
}

template <typename Interface> void Scheduler<Interface>::orderedInsert(const Event &evt) {
	this->eventQueue.push_back(evt);
	if (timespecLt(evt.time(), this->eventQueue.back().time())) { //If not already ordered, we must order it.
		std::push_heap(this->eventQueue.begin(), this->eventQueue.end());
	}
}

template <typename Interface> void Scheduler<Interface>::schedPwm(AxisIdType idx, const PwmInfo &p) {
	LOGV("Scheduler::schedPwm: %i, %u, %u. Current: %u, %u\n", idx, p.nsHigh, p.nsLow, pwmInfo[idx].nsHigh, pwmInfo[idx].nsLow);
	if (pwmInfo[idx].nsHigh != 0 || pwmInfo[idx].nsLow != 0) { //already scheduled and running. Just update times.
		pwmInfo[idx] = p;
	} else { //have to schedule:
		LOGV("Scheduler::schedPwm: queueing\n");
		pwmInfo[idx] = p;
		Event evt(timespecNow(), idx, p.nsHigh ? StepForward : StepBackward); //if we have any high-time, then start with forward, else backward.
		this->queue(evt);
	}
}


template <typename Interface> Event Scheduler<Interface>::nextEvent(bool doSleep, std::chrono::microseconds timeout) {
	Event evt;
	{
		std::unique_lock<std::mutex> lock(this->mutex);
		while (this->eventQueue.empty()) { //wait for an event to be pushed.
			//condition_variable.wait() can produce spurious wakeups; need the while loop.
			//this->nonemptyCond.wait(_lockPushes); //condition_variable.wait() can produce spurious wakeups; need the while loop.
			if (this->nonemptyCond.wait_for(lock, timeout) == std::cv_status::timeout) { 
				return Event(); //return null event
			}
		}
		evt = this->eventQueue.front();
		this->eventQueue.pop_front();
		//check if event is PWM-based:
		//if (pwmInfo[evt.stepperId()].nsLow != 0 || pwmInfo[evt.stepperId()].nsHigh != 0) {
		if (pwmInfo[evt.stepperId()].isNonNull()) {
			if (evt.direction() == StepForward) {
				//next event will be StepBackward, or refresh this event if there is no off-duty.
				Event nextPwm(evt.time(), evt.stepperId(), pwmInfo[evt.stepperId()].nsLow ? StepBackward : StepForward);
				nextPwm.offsetNano(pwmInfo[evt.stepperId()].nsHigh);
				this->orderedInsert(nextPwm); //to do: ordered insert
			} else {
				//next event will be StepForward, or refresh this event if there is no on-duty.
				Event nextPwm(evt.time(), evt.stepperId(), pwmInfo[evt.stepperId()].nsHigh ? StepForward : StepBackward);
				nextPwm.offsetNano(pwmInfo[evt.stepperId()].nsLow);
				this->orderedInsert(nextPwm);
			}
		} else { //non-pwm event, means the queue size has decreased by 1.
			lock.unlock();
			eventConsumedCond.notify_one();
		}
	}
	if (doSleep) {
		this->sleepUntilEvent(evt);
		/*struct timespec sleepUntil = evt.time();
		//struct timespec curTime;
		//clock_gettime(CLOCK_MONOTONIC, &curTime);
		//LOGV("Scheduler::nextEvent sleep from %lu.%lu until %lu.%lu\n", curTime.tv_sec, curTime.tv_nsec, sleepUntil.tv_sec, sleepUntil.tv_nsec);
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &sleepUntil, NULL); //sleep to event time.
		//clock_gettime(CLOCK_MONOTONIC, &(this->lastEventHandledTime)); //in case we fall behind, preserve the relative time between events.
		//this->lastEventHandledTime = sleepUntil;*/
	}
	
	return evt;
}

template <typename Interface> void Scheduler<Interface>::sleepUntilEvent(const Event &evt) const {
	struct timespec sleepUntil = evt.time();
	clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &sleepUntil, NULL); //sleep to event time.
}

template <typename Interface> void Scheduler<Interface>::initSchedThread() const {
	struct sched_param sp; 
	sp.sched_priority=SCHED_PRIORITY; 
	if (int ret = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp)) {
		LOGW("Warning: pthread_setschedparam (increase thread priority) at scheduler.cpp returned non-zero: %i\n", ret);
	}
}

template <typename Interface> struct timespec Scheduler<Interface>::lastSchedTime() const {
	Event evt;
	{
		std::unique_lock<std::mutex> lock(this->mutex);
		if (this->eventQueue.size()) {
			evt = this->eventQueue.back();
		} else {
			lock.unlock();
			timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			return ts;
		}
	}
	return evt.time();
}

template <typename Interface> void Scheduler<Interface>::setBufferSize(unsigned size) {
	this->bufferSize = size;
	LOG("Scheduler buffer size set: %u\n", size);
}
template <typename Interface> unsigned Scheduler<Interface>::getBufferSize() const {
	return this->bufferSize;
}

template <typename Interface> unsigned Scheduler<Interface>::numActivePwmChannels() const {
	unsigned r=0;
	for (const PwmInfo &p : this->pwmInfo) {
		if (p.isNonNull()) {
			r += 1;
		}
	}
	return r;
}

#endif
