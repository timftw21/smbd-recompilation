#pragma once
#include <chrono>
#include <cstdint>

namespace smbd {
// Independent absolute deadlines, carrying fractional nanoseconds at rates
// such as 60/144 Hz. Fixed rule ticks keep their deadlines; native scene
// rendering can discard overdue display slots while retaining elapsed time.
class FrameClock {
public:
    using Clock=std::chrono::steady_clock;
    using Time=Clock::time_point;
    bool due(unsigned rate,Time now) {
        if(!started_ || rate!=rate_) {rate_=rate;next_=now;remainder_=0;started_=true;}
        return !rate_ || now>=next_;
    }
    void advance(Time now) {
        if(!rate_) {next_=now;return;}
        remainder_+=1000000000U%rate_;
        next_+=std::chrono::nanoseconds(1000000000U/rate_+remainder_/rate_);
        remainder_%=rate_;
    }
    void skip_missed(Time now) {
        if(rate_ && next_<=now) {next_=now;remainder_=0;advance(now);}
    }
    Time next() const {return next_;}
    void discard_overrun(Time now) {
        if(rate_ && now-next_>std::chrono::nanoseconds(1000000000U/rate_)) {next_=now;remainder_=0;}
    }
private:
    unsigned rate_=0,remainder_=0;
    Time next_{};
    bool started_=false;
};
}
