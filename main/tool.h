#ifndef TOOL_H
#define TOOL_H

#include <string>
#include <functional>
#include <map>
#include <vector>
#include <cJSON.h>

struct ToolCommand {
    std::string name;
    std::string method;
    cJSON* parameters;
};

using ToolCallback = std::function<bool(const ToolCommand& cmd, std::string& result)>;

class ToolManager {
public:
    static ToolManager& GetInstance();

    void RegisterTool(const std::string& name, ToolCallback callback);

    bool ExecuteToolCommand(const cJSON* json, std::string& result, std::string& tool_id);

    bool ParseToolMessage(const char* data, size_t len, std::string& result);

    static std::string BuildToolResponse(const std::string& tool_id, bool success, const std::string& result);

    void SendToolResponse(const std::string& tool_id, bool success, const std::string& result);

    void RegisterBuiltinTools();

private:
    ToolManager() = default;
    ~ToolManager() = default;

    std::map<std::string, ToolCallback> tools_;
};

#endif // TOOL_H
