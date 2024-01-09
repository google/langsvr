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

#ifndef LANGSVR_SESSION_H_
#define LANGSVR_SESSION_H_

#include <functional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "langsvr/json/builder.h"
#include "langsvr/json/value.h"
#include "langsvr/lsp/message_kind.h"
#include "langsvr/result.h"

namespace langsvr {

/// Session provides a message dispatch registry for LSP messages.
class Session {
    struct RequestHandler {
        std::function<Result<const json::Value*>(const json::Value&, json::Builder&)> function;
        std::function<void()> post_send;
    };
    struct NotificationHandler {
        std::function<Result<SuccessType>(const json::Value&)> function;
    };

  public:
    using Sender = std::function<Result<SuccessType>(std::string_view)>;

    /// SetSender sets the message send handler used by Session for sending request responses and
    /// notifications.
    /// @param sender the new sender for the session.
    void SetSender(Sender&& sender) { sender_ = std::move(sender); }

    /// Receive decodes the LSP message from the JSON string @p json, calling the appropriate
    /// registered message handler, and sending the response to the registered Sender if the message
    /// was an LSP request.
    /// @param json the incoming JSON message.
    /// @return success or failure
    Result<SuccessType> Receive(std::string_view json);

    /// Send encodes and sends the LSP request or notification to the Sender registered with
    /// SetSender.
    /// @param message the Request or Notification message
    /// @return success or failure
    template <typename T>
    Result<SuccessType> Send(T&& message) {
        using Message = std::decay_t<T>;
        static constexpr bool kIsRequest = Message::kMessageKind == lsp::MessageKind::kRequest;
        static constexpr bool kIsNotification =
            Message::kMessageKind == lsp::MessageKind::kNotification;
        static_assert(kIsRequest || kIsNotification);

        auto b = json::Builder::Create();
        std::vector<json::Builder::Member> members{
            json::Builder::Member{"method", b->String(Message::kMethod)},
        };
        if constexpr (Message::kHasParams) {
            auto params = Encode(message, *b.get());
            if (params != Success) {
                return params.Failure();
            }
            members.push_back(json::Builder::Member{"params", params.Get()});
        }
        return SendJson(b->Object(members)->Json());
    }

    /// RegisteredRequestHandler is the return type Register() when registering a Request hander.
    class RegisteredRequestHandler {
      public:
        /// OnPostSend registers @p callback to be called once the request response has been sent.
        /// @param callback the callback function to call when a response has been sent.
        void OnPostSend(std::function<void()>&& callback) {
            handler.post_send = std::move(callback);
        }

      private:
        friend class Session;
        RegisteredRequestHandler(RequestHandler& h) : handler{h} {}
        RegisteredRequestHandler(const RegisteredRequestHandler&) = delete;
        RegisteredRequestHandler& operator=(const RegisteredRequestHandler&) = delete;
        RequestHandler& handler;
    };

    /// Register registers the LSP Request or Notification handler to be called when Receive() is
    /// called with a message of the appropriate type.
    /// @tparam F a function with the signature `Result<RESPONSE>(const REQUEST&)` or
    /// `Result<SuccessType>(const NOTIFICATION&)`
    /// @return a RegisteredRequestHandler if the parameter type of F is a LSP request, otherwise
    /// void.
    template <typename F>
    auto Register(F&& callback) {
        // Examine the function signature to determine the message type
        using Sig = SignatureOf<F>;
        static_assert(Sig::parameter_count == 1);
        using Message = typename Sig::template parameter<0>;

        // Is the message a request or notification?
        static constexpr bool kIsRequest = Message::kMessageKind == lsp::MessageKind::kRequest;
        static constexpr bool kIsNotification =
            Message::kMessageKind == lsp::MessageKind::kNotification;
        static_assert(kIsRequest || kIsNotification);

        // The method is the identification string used in the JSON message.
        auto method = std::string(Message::kMethod);

        if constexpr (kIsRequest) {
            auto& handler = request_handlers_[method];
            handler.function = [f = std::move(callback)](
                                   const json::Value& object,
                                   json::Builder& json_builder) -> Result<const json::Value*> {
                Message request;
                if constexpr (Message::kHasParams) {
                    auto params = object.Get("params");
                    if (params != Success) {
                        return params.Failure();
                    }
                    if (auto res = Decode(*params.Get(), request); res != Success) {
                        return res.Failure();
                    }
                }
                // TODO(bclayton): Support Message::ErrorData failure types.
                Result<typename Message::Result> res = f(request);
                if (res != Success) {
                    return res.Failure();
                }
                return Encode(res.Get(), json_builder);
            };
            return RegisteredRequestHandler{handler};
        } else if constexpr (kIsNotification) {
            auto& handler = notification_handlers_[method];
            handler.function =
                [f = std::move(callback)](const json::Value& object) -> Result<SuccessType> {
                Message notification;
                if constexpr (Message::kHasParams) {
                    auto params = object.Get("params");
                    if (params != Success) {
                        return params.Failure();
                    }
                    if (auto res = Decode(*params.Get(), notification); res != Success) {
                        return res.Failure();
                    }
                }
                return f(notification);
            };
            return;
        }
    }

  private:
    Result<SuccessType> SendJson(std::string_view msg);

    Sender sender_;
    std::unordered_map<std::string, RequestHandler> request_handlers_;
    std::unordered_map<std::string, NotificationHandler> notification_handlers_;
};

}  // namespace langsvr

#endif  // LANGSVR_SESSION_H_
