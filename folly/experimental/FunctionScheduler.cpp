/*
 * Copyright 2015 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/experimental/FunctionScheduler.h>
#include <folly/ThreadName.h>
#include <folly/Conv.h>
#include <folly/String.h>

using namespace std;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

namespace folly {

FunctionScheduler::FunctionScheduler() {
}

FunctionScheduler::~FunctionScheduler() {
  // make sure to stop the thread (if running)
  shutdown();
}

void FunctionScheduler::addFunction(const std::function<void()>& cb,
                                    milliseconds interval,
                                    StringPiece nameID,
                                    milliseconds startDelay) {
  LatencyDistribution latencyDistr(false, 0.0);
  addFunctionInternal(cb, interval,
                      latencyDistr, nameID, startDelay);
}

void FunctionScheduler::addFunctionInternal(const std::function<void()>& cb,
                                    milliseconds interval,
                                    const LatencyDistribution& latencyDistr,
                                    StringPiece nameID,
                                    milliseconds startDelay) {
  if (interval < milliseconds::zero()) {
    throw std::invalid_argument("FunctionScheduler: "
                                "time interval must be non-negative");
  }
  if (startDelay < milliseconds::zero()) {
    throw std::invalid_argument("FunctionScheduler: "
                                "start delay must be non-negative");
  }

  std::lock_guard<std::mutex> l(mutex_);
  // check if the nameID is unique
  for (const auto& f : functions_) {
    if (f.isValid() && f.name == nameID) {
      throw std::invalid_argument(to<string>(
            "FunctionScheduler: a function named \"", nameID,
            "\" already exists"));
    }
  }
  if (currentFunction_ && currentFunction_->name == nameID) {
    throw std::invalid_argument(to<string>(
          "FunctionScheduler: a function named \"", nameID,
          "\" already exists"));
  }

  functions_.emplace_back(cb, interval, nameID.str(), startDelay,
                          latencyDistr.isPoisson, latencyDistr.poissonMean);
  if (running_) {
    functions_.back().setNextRunTime(steady_clock::now() + startDelay);
    std::push_heap(functions_.begin(), functions_.end(), fnCmp_);
    // Signal the running thread to wake up and see if it needs to change it's
    // current scheduling decision.
    runningCondvar_.notify_one();
  }
}

bool FunctionScheduler::cancelFunction(StringPiece nameID) {
  std::unique_lock<std::mutex> l(mutex_);

  if (currentFunction_ && currentFunction_->name == nameID) {
    // This function is currently being run.  Clear currentFunction_
    // The running thread will see this and won't reschedule the function.
    currentFunction_ = nullptr;
    return true;
  }

  for (auto it = functions_.begin(); it != functions_.end(); ++it) {
    if (it->isValid() && it->name == nameID) {
      cancelFunction(l, it);
      return true;
    }
  }
  return false;
}

void FunctionScheduler::cancelFunction(const std::unique_lock<std::mutex>& l,
                                       FunctionHeap::iterator it) {
  // This function should only be called with mutex_ already locked.
  DCHECK(l.mutex() == &mutex_);
  DCHECK(l.owns_lock());

  if (running_) {
    // Internally gcc has an __adjust_heap() function to fill in a hole in the
    // heap.  Unfortunately it isn't part of the standard API.
    //
    // For now we just leave the RepeatFunc in our heap, but mark it as unused.
    // When it's nextTimeInterval comes up, the runner thread will pop it from
    // the heap and simply throw it away.
    it->cancel();
  } else {
    // We're not running, so functions_ doesn't need to be maintained in heap
    // order.
    functions_.erase(it);
  }
}

void FunctionScheduler::cancelAllFunctions() {
  std::unique_lock<std::mutex> l(mutex_);
  functions_.clear();
}

bool FunctionScheduler::start() {
  std::unique_lock<std::mutex> l(mutex_);
  if (running_) {
    return false;
  }

  running_ = true;

  VLOG(1) << "Starting FunctionScheduler with " << functions_.size()
          << " functions.";
  auto now = steady_clock::now();
  // Reset the next run time. for all functions.
  // note: this is needed since one can shutdown() and start() again
  for (auto& f : functions_) {
    f.setNextRunTime(now + f.startDelay);
    VLOG(1) << "   - func: "
            << (f.name.empty() ? "(anon)" : f.name.c_str())
            << ", period = " << f.timeInterval.count()
            << "ms, delay = " << f.startDelay.count() << "ms";
  }
  std::make_heap(functions_.begin(), functions_.end(), fnCmp_);

  thread_ = std::thread([&] { this->run(); });
  return true;
}

void FunctionScheduler::shutdown() {
  {
    std::lock_guard<std::mutex> g(mutex_);
    if (!running_) {
      return;
    }

    running_ = false;
    runningCondvar_.notify_one();
  }
  thread_.join();
}

void FunctionScheduler::run() {
  std::unique_lock<std::mutex> lock(mutex_);

  if (!threadName_.empty()) {
    folly::setThreadName(threadName_);
  }

  while (running_) {
    // If we have nothing to run, wait until a function is added or until we
    // are stopped.
    if (functions_.empty()) {
      runningCondvar_.wait(lock);
      continue;
    }

    auto now = steady_clock::now();

    // Move the next function to run to the end of functions_
    std::pop_heap(functions_.begin(), functions_.end(), fnCmp_);

    // Check to see if the function was cancelled.
    // If so, just remove it and continue around the loop.
    if (!functions_.back().isValid()) {
      functions_.pop_back();
      continue;
    }

    auto sleepTime = functions_.back().getNextRunTime() - now;
    if (sleepTime < milliseconds::zero()) {
      // We need to run this function now
      runOneFunction(lock, now);
    } else {
      // Re-add the function to the heap, and wait until we actually
      // need to run it.
      std::push_heap(functions_.begin(), functions_.end(), fnCmp_);
      runningCondvar_.wait_for(lock, sleepTime);
    }
  }
}

void FunctionScheduler::runOneFunction(std::unique_lock<std::mutex>& lock,
                                       steady_clock::time_point now) {
  DCHECK(lock.mutex() == &mutex_);
  DCHECK(lock.owns_lock());

  // The function to run will be at the end of functions_ already.
  //
  // Fully remove it from functions_ now.
  // We need to release mutex_ while we invoke this function, and we need to
  // maintain the heap property on functions_ while mutex_ is unlocked.
  RepeatFunc func(std::move(functions_.back()));
  functions_.pop_back();
  currentFunction_ = &func;

  // Update the function's run time, and re-insert it into the heap.
  if (steady_) {
    // This allows scheduler to catch up
    func.lastRunTime += func.timeInterval;
  } else {
    // Note that we adjust lastRunTime to the current time where we started the
    // function call, rather than the time when the function finishes.
    // This ensures that we call the function once every time interval, as
    // opposed to waiting time interval seconds between calls.  (These can be
    // different if the function takes a significant amount of time to run.)
    func.lastRunTime = now;
  }

  // Release the lock while we invoke the user's function
  lock.unlock();

  // Invoke the function
  try {
    VLOG(5) << "Now running " << func.name;
    func.cb();
  } catch (const std::exception& ex) {
    LOG(ERROR) << "Error running the scheduled function <"
      << func.name << ">: " << exceptionStr(ex);
  }

  // Re-acquire the lock
  lock.lock();

  if (!currentFunction_) {
    // The function was cancelled while we were running it.
    // We shouldn't reschedule it;
    return;
  }
  // Clear currentFunction_
  CHECK_EQ(currentFunction_, &func);
  currentFunction_ = nullptr;

  // Re-insert the function into our functions_ heap.
  // We only maintain the heap property while running_ is set.  (running_ may
  // have been cleared while we were invoking the user's function.)
  if (func.isPoissonDistr) {
    func.setTimeIntervalPoissonDistr();
  }
  functions_.push_back(std::move(func));
  if (running_) {
    std::push_heap(functions_.begin(), functions_.end(), fnCmp_);
  }
}

void FunctionScheduler::setThreadName(StringPiece threadName) {
  std::unique_lock<std::mutex> l(mutex_);
  threadName_ = threadName.str();
}

}
