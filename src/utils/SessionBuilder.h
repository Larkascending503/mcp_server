#ifndef MCP_SERVER_SESSIONBUILDER_H
#define MCP_SERVER_SESSIONBUILDER_H 

#include <chrono>
#include <random>
#include <sstream>
#include <string>
#include <iomanip>

namespace vx::utils {

    class SessionBuilder {
    public:
        static std::string GenerateUniqueSessionID(){
            auto now = std::chrono::high_resolution_clock::now();
            auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch()).count();
                static std::random_device rd;
                static std::mt19937 gen(rd());
                static std::uniform_int_distribution<uint32_t> dis;

                std::stringstream ss;
                ss << std::hex << timestamp << "-" << dis(gen);
                return ss.str();
        }
    };

}

#endif