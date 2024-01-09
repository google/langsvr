// Copyright 2024 The langsvr Authors
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "langsvr/session.h"
#include "langsvr/json/builder.h"

namespace langsvr {

Result<SuccessType> Session::Receive(std::string_view json) {
    auto json_builder = json::Builder::Create();
    auto object = json_builder->Parse(json);
    if (object != Success) {
        return object.Failure();
    }

    auto method = object.Get()->Get<json::String>("method");
    if (method != Success) {
        return method.Failure();
    }

    if (object.Get()->Has("id")) {  // Request
        auto id = object.Get()->Get<json::I64>("id");
        if (id != Success) {
            return id.Failure();
        }

        auto it = request_handlers_.find(method.Get());
        if (it == request_handlers_.end()) {
            return Failure{"no handler registered for request method '" + method.Get() + "'"};
        }
        auto& request_handler = it->second;

        std::vector response_members{
            json::Builder::Member{"id", json_builder->I64(id.Get())},
        };

        if (auto result = request_handler.function(*object.Get(), *json_builder.get());
            result == Success) {
            if (auto* result_json = result.Get()) {
                response_members.push_back(json::Builder::Member{"result", result.Get()});
            }
        } else {
            auto err = result.Failure().reason;
            response_members.push_back(json::Builder::Member{"error", json_builder->String(err)});
        }

        auto* response = json_builder->Object(response_members);
        if (auto res = SendJson(response->Json()); res != Success) {
            return res.Failure();
        }

        if (request_handler.post_send) {
            request_handler.post_send();
        }
    } else {  // Notification
        auto it = notification_handlers_.find(method.Get());
        if (it == notification_handlers_.end()) {
            return Failure{"no handler registered for request method '" + method.Get() + "'"};
        }
        auto& notification_handler = it->second;
        return notification_handler.function(*object.Get());
    }

    return Success;
}

Result<SuccessType> Session::SendJson(std::string_view msg) {
    if (!sender_) [[unlikely]] {
        return Failure{"no sender set"};
    }
    return sender_(msg);
}

}  // namespace langsvr
