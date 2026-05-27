#pragma once

#include <mutex>
#include <string>
#include <thread>

namespace psvr2pt {

class UpdateChecker {
public:
    enum class State { Pending, UpToDate, Available, Failed };

    ~UpdateChecker();
    void start(const char* current_version);
    void shutdown();

    State       state()      const;
    std::string latest_tag() const;

private:
    void run(std::string current_version);

    std::thread        thread_;
    mutable std::mutex mutex_;
    State              state_      = State::Pending;
    std::string        latest_tag_;
};

}  // namespace psvr2pt
