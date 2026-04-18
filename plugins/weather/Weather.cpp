#include "PluginAPI.h"
#include "json.hpp"
#include "httplib.h"

using json = nlohmann::json;

static PluginTool methods[] {
    {
        "get_weather",
        "Get weather forecast of a city in the world. just pass as parameter the latitude and longitude of the city you want to know the weather forecast.",
        R"({
            "$schema": "http://json-schema.org/draft-07/schema#",
            "type": "object",
            "properties": {
                "latitude": {"type": "string"},
                "longitude": {"type": "string"},
                "city": {"type": "string"}
            }, 
            "required": ["latitude", "longitude", "city"],
            "additionalProperties": false
        })"
    }
};

const char* GetNameImpl(){return "weather-tools";}
const char* GetVersionImpl(){return "1.0.0";}
PluginType GetTypeImpl(){return PLUGIN_TYPE_TOOLS;}

int InitializeImpl(){
    return 1;
}

char* HandleRequestImpl(const char* req){
    auto request = json::parse(req);

    auto latitude = request["params"]["arguments"]["latitude"].get<std::string>();
    auto longitude = request["params"]["arguments"]["longitude"].get<std::string>();
    auto city = request["params"]["arguments"]["city"].get<std::string>();

    json weatherContent;

    httplib::Client cli("api.open-meteo.com");
    auto res = cli.Get("/v1/forecast?latitude=" + latitude + "&longitude=" + longitude + "&hourly=temperature_2m&forecast_days=1");
    if(res && res->status == 200){
        auto weatherData = json::parse(res->body);

        auto city = request["params"]["arguments"]["city"].get<std::string>();

        std::stringstream weatherMessage;
        weatherMessage << "Weather Forecast for " << city << ":\n\n";

        auto times = weatherData["hourly"]["time"];
        auto temperatures = weatherData["hourly"]["temperature_2m"];

        weatherMessage << "Today's Temperature Forecast:\n";

        weatherMessage << "🌅 Morning: ";
        double morningTemp = 0.0;
        int morningCount = 0;
        for (int i = 6; i < 12; i++) {
            morningTemp += temperatures[i].get<double>();
            morningCount++;
        }
        morningTemp /= morningCount;
        weatherMessage << std::fixed << std::setprecision(1) << morningTemp << "°C\n";

        weatherMessage << "☀️ Afternoon: ";
        double afternoonTemp = 0.0;
        int afternoonCount = 0;
        for (int i = 12; i < 18; i++) {
            afternoonTemp += temperatures[i].get<double>();
            afternoonCount++;
        }
        afternoonTemp /= afternoonCount;
        weatherMessage << std::fixed << std::setprecision(1) << afternoonTemp << "°C\n";

        weatherMessage << "🌙 Evening: ";
        double eveningTemp = 0.0;
        int eveningCount = 0;
        for (int i = 18; i < 24; i++) {
            eveningTemp += temperatures[i].get<double>();
            eveningCount++;
        }
        eveningTemp /= eveningCount;
        weatherMessage << std::fixed << std::setprecision(1) << eveningTemp << "°C\n\n";

        double maxTemp = temperatures[0].get<double>();
        double minTemp = temperatures[0].get<double>();
        std::string maxTime, minTime;

        for (size_t i = 0; i < temperatures.size(); i++) {
            double temp = temperatures[i].get<double>();
            if (temp > maxTemp) {
                maxTemp = temp;
                maxTime = times[i].get<std::string>().substr(11, 5); // Extract HH:MM
            }
            if (temp < minTemp) {
                minTemp = temp;
                minTime = times[i].get<std::string>().substr(11, 5); // Extract HH:MM
            }
        }

        weatherMessage << "🔼 Highest: " << std::fixed << std::setprecision(1) << maxTemp << "°C at " << maxTime << "\n";
        weatherMessage << "🔽 Lowest: " << std::fixed << std::setprecision(1) << minTemp << "°C at " << minTime << "\n\n";

        weatherMessage << "📊 Daily Summary: ";
        if (maxTemp > 25) {
            weatherMessage << "Hot day! ";
        } else if (maxTemp > 20) {
            weatherMessage << "Warm day. ";
        } else if (maxTemp > 10) {
            weatherMessage << "Mild temperatures. ";
        } else {
            weatherMessage << "Cool day. ";
        }

        double tempVariation = maxTemp - minTemp;
        if (tempVariation > 10) {
            weatherMessage << "Large temperature variation throughout the day.";
        } else if (tempVariation > 5) {
            weatherMessage << "Moderate temperature changes expected.";
        } else {
            weatherMessage << "Fairly consistent temperatures today.";
        }

        weatherContent["type"] = "text";
        weatherContent["text"] = weatherMessage.str();
    } else {
        weatherContent["type"] = "text";
        weatherContent["text"] = "Error fetching weather data.";
    }

    json response;
    response["content"] = json::array();
    response["content"].push_back(weatherContent);
    response["isError"] = false;

    std::string result = response.dump();
    char* buffer = new char[result.length() + 1];
    strcpy(buffer, result.c_str());
    return buffer;
}

void ShutdownImpl(){   
}

int GetToolCountImpl(){
    return sizeof(methods) / sizeof(methods[0]);
}

const PluginTool* GetToolImpl(int index){
    if(index < 0 || index >= GetToolCountImpl()) return nullptr;
    return &methods[index];
}

static PluginAPI plugin = {
        GetNameImpl,
        GetVersionImpl,
        GetTypeImpl,
        InitializeImpl,
        HandleRequestImpl,
        ShutdownImpl,
        GetToolCountImpl,
        GetToolImpl,
        nullptr,
        nullptr,
        nullptr,
        nullptr
};

extern "C" PLUGIN_API PluginAPI* CreatePlugin(){
    return &plugin;
}

extern "C" PLUGIN_API void DestroyPlugin(PluginAPI*) {
}