// src/sbcli_mcp.hpp —— MCP server（stdio JSON-RPC 2.0，行模式）
//
// 让 AI 直接调用 sbcli 的能力，不必经过 shell。
// 协议：行模式 —— 每行一个 JSON-RPC 请求，每行一个紧凑 JSON 响应。
// stdout 只写 JSON（日志一律走 stderr），否则会破坏协议。
#pragma once

#include "commands.hpp"

#include <iosfwd>
#include <string>

namespace sb {

// 处理一行请求，返回一行响应。
// 返回空串 = 不该有响应（通知 / 空行 / Content-Length 头行）。
std::string mcpHandleLine(const std::string& line);

// 逐行读请求、逐行写响应；stdin 读到 EOF 正常返回。
void mcpServe(std::istream& in, std::ostream& out);

// sb mcp —— 进入 MCP server 循环，读 stdin 直到 EOF。
int cmd_mcp(Args& a);

} // namespace sb
