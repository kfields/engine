// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "flutter/shell/platform/windows/text_input_plugin.h"

#include <rapidjson/document.h>
#include <windows.h>
#include <memory>

#include "flutter/fml/macros.h"
#include "flutter/shell/platform/common/json_message_codec.h"
#include "flutter/shell/platform/common/json_method_codec.h"
#include "flutter/shell/platform/windows/testing/test_binary_messenger.h"
#include "flutter/shell/platform/windows/text_input_plugin_delegate.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace flutter {
namespace testing {

namespace {
static constexpr char kScanCodeKey[] = "scanCode";
static constexpr int kHandledScanCode = 20;
static constexpr int kUnhandledScanCode = 21;
static constexpr char kTextPlainFormat[] = "text/plain";
static constexpr int kDefaultClientId = 42;
// Should be identical to constants in text_input_plugin.cc.
static constexpr char kChannelName[] = "flutter/textinput";
static constexpr char kEnableDeltaModel[] = "enableDeltaModel";
static constexpr char kSetClientMethod[] = "TextInput.setClient";
static constexpr char kAffinityDownstream[] = "TextAffinity.downstream";
static constexpr char kTextKey[] = "text";
static constexpr char kSelectionBaseKey[] = "selectionBase";
static constexpr char kSelectionExtentKey[] = "selectionExtent";
static constexpr char kSelectionAffinityKey[] = "selectionAffinity";
static constexpr char kSelectionIsDirectionalKey[] = "selectionIsDirectional";
static constexpr char kComposingBaseKey[] = "composingBase";
static constexpr char kComposingExtentKey[] = "composingExtent";

static std::unique_ptr<std::vector<uint8_t>> CreateResponse(bool handled) {
  auto response_doc =
      std::make_unique<rapidjson::Document>(rapidjson::kObjectType);
  auto& allocator = response_doc->GetAllocator();
  response_doc->AddMember("handled", handled, allocator);
  return JsonMessageCodec::GetInstance().EncodeMessage(*response_doc);
}

static std::unique_ptr<rapidjson::Document> EncodedClientConfig(
    std::string type_name,
    std::string input_action) {
  auto arguments = std::make_unique<rapidjson::Document>(rapidjson::kArrayType);
  auto& allocator = arguments->GetAllocator();
  arguments->PushBack(kDefaultClientId, allocator);

  rapidjson::Value config(rapidjson::kObjectType);
  config.AddMember("inputAction", input_action, allocator);
  config.AddMember(kEnableDeltaModel, false, allocator);
  rapidjson::Value type_info(rapidjson::kObjectType);
  type_info.AddMember("name", type_name, allocator);
  config.AddMember("inputType", type_info, allocator);
  arguments->PushBack(config, allocator);

  return arguments;
}

static std::unique_ptr<rapidjson::Document> EncodedEditingState(
    std::string text,
    TextRange selection) {
  auto model = std::make_unique<TextInputModel>();
  model->SetText(text);
  model->SetSelection(selection);

  auto arguments = std::make_unique<rapidjson::Document>(rapidjson::kArrayType);
  auto& allocator = arguments->GetAllocator();
  arguments->PushBack(kDefaultClientId, allocator);

  rapidjson::Value editing_state(rapidjson::kObjectType);
  editing_state.AddMember(kSelectionAffinityKey, kAffinityDownstream,
                          allocator);
  editing_state.AddMember(kSelectionBaseKey, selection.base(), allocator);
  editing_state.AddMember(kSelectionExtentKey, selection.extent(), allocator);
  editing_state.AddMember(kSelectionIsDirectionalKey, false, allocator);

  int composing_base =
      model->composing() ? model->composing_range().base() : -1;
  int composing_extent =
      model->composing() ? model->composing_range().extent() : -1;
  editing_state.AddMember(kComposingBaseKey, composing_base, allocator);
  editing_state.AddMember(kComposingExtentKey, composing_extent, allocator);
  editing_state.AddMember(kTextKey,
                          rapidjson::Value(model->GetText(), allocator).Move(),
                          allocator);
  arguments->PushBack(editing_state, allocator);

  return arguments;
}

class MockTextInputPluginDelegate : public TextInputPluginDelegate {
 public:
  MockTextInputPluginDelegate() {}
  virtual ~MockTextInputPluginDelegate() = default;

  MOCK_METHOD(void, OnCursorRectUpdated, (const Rect&), (override));
  MOCK_METHOD(void, OnResetImeComposing, (), (override));

 private:
  FML_DISALLOW_COPY_AND_ASSIGN(MockTextInputPluginDelegate);
};

}  // namespace

TEST(TextInputPluginTest, TextMethodsWorksWithEmptyModel) {
  auto handled_message = CreateResponse(true);
  auto unhandled_message = CreateResponse(false);
  int received_scancode = 0;

  TestBinaryMessenger messenger(
      [&received_scancode, &handled_message, &unhandled_message](
          const std::string& channel, const uint8_t* message,
          size_t message_size, BinaryReply reply) {});
  MockTextInputPluginDelegate delegate;

  int redispatch_scancode = 0;
  TextInputPlugin handler(&messenger, &delegate);

  handler.KeyboardHook(VK_RETURN, 100, WM_KEYDOWN, '\n', false, false);
  handler.ComposeBeginHook();
  std::u16string text;
  text.push_back('\n');
  handler.ComposeChangeHook(text, 1);
  handler.ComposeEndHook();

  // Passes if it did not crash
}

TEST(TextInputPluginTest, ClearClientResetsComposing) {
  TestBinaryMessenger messenger([](const std::string& channel,
                                   const uint8_t* message, size_t message_size,
                                   BinaryReply reply) {});
  BinaryReply reply_handler = [](const uint8_t* reply, size_t reply_size) {};

  MockTextInputPluginDelegate delegate;
  TextInputPlugin handler(&messenger, &delegate);

  EXPECT_CALL(delegate, OnResetImeComposing());

  auto& codec = JsonMethodCodec::GetInstance();
  auto message = codec.EncodeMethodCall({"TextInput.clearClient", nullptr});
  messenger.SimulateEngineMessage(kChannelName, message->data(),
                                  message->size(), reply_handler);
}

// Verify that the embedder sends state update messages to the framework during
// IME composing.
TEST(TextInputPluginTest, VerifyComposingSendStateUpdate) {
  bool sent_message = false;
  TestBinaryMessenger messenger(
      [&sent_message](const std::string& channel, const uint8_t* message,
                      size_t message_size,
                      BinaryReply reply) { sent_message = true; });
  BinaryReply reply_handler = [](const uint8_t* reply, size_t reply_size) {};

  MockTextInputPluginDelegate delegate;
  TextInputPlugin handler(&messenger, &delegate);

  auto& codec = JsonMethodCodec::GetInstance();

  // Call TextInput.setClient to initialize the TextInputModel.
  auto arguments = std::make_unique<rapidjson::Document>(rapidjson::kArrayType);
  auto& allocator = arguments->GetAllocator();
  arguments->PushBack(kDefaultClientId, allocator);
  rapidjson::Value config(rapidjson::kObjectType);
  config.AddMember("inputAction", "done", allocator);
  config.AddMember("inputType", "text", allocator);
  config.AddMember(kEnableDeltaModel, false, allocator);
  arguments->PushBack(config, allocator);
  auto message =
      codec.EncodeMethodCall({"TextInput.setClient", std::move(arguments)});
  messenger.SimulateEngineMessage("flutter/textinput", message->data(),
                                  message->size(), reply_handler);

  // ComposeBeginHook should send state update.
  sent_message = false;
  handler.ComposeBeginHook();
  EXPECT_TRUE(sent_message);

  // ComposeChangeHook should send state update.
  sent_message = false;
  handler.ComposeChangeHook(u"4", 1);
  EXPECT_TRUE(sent_message);

  // ComposeCommitHook should NOT send state update.
  //
  // Commit messages are always immediately followed by a change message or an
  // end message, both of which will send an update. Sending intermediate state
  // with a collapsed composing region will trigger the framework to assume
  // composing has ended, which is not the case until a WM_IME_ENDCOMPOSING
  // event is received in the main event loop, which will trigger a call to
  // ComposeEndHook.
  sent_message = false;
  handler.ComposeCommitHook();
  EXPECT_FALSE(sent_message);

  // ComposeEndHook should send state update.
  sent_message = false;
  handler.ComposeEndHook();
  EXPECT_TRUE(sent_message);
}

TEST(TextInputPluginTest, VerifyInputActionNewlineInsertNewLine) {
  // Store messages as std::string for convenience.
  std::vector<std::string> messages;

  TestBinaryMessenger messenger(
      [&messages](const std::string& channel, const uint8_t* message,
                  size_t message_size, BinaryReply reply) {
        std::string last_message(reinterpret_cast<const char*>(message),
                                 message_size);
        messages.push_back(last_message);
      });
  BinaryReply reply_handler = [](const uint8_t* reply, size_t reply_size) {};

  MockTextInputPluginDelegate delegate;
  TextInputPlugin handler(&messenger, &delegate);

  auto& codec = JsonMethodCodec::GetInstance();

  // Call TextInput.setClient to initialize the TextInputModel.
  auto set_client_arguments =
      EncodedClientConfig("TextInputType.multiline", "TextInputAction.newline");
  auto message = codec.EncodeMethodCall(
      {"TextInput.setClient", std::move(set_client_arguments)});
  messenger.SimulateEngineMessage("flutter/textinput", message->data(),
                                  message->size(), reply_handler);

  // Simulate a key down event for '\n'.
  handler.KeyboardHook(VK_RETURN, 100, WM_KEYDOWN, '\n', false, false);

  // Two messages are expected, the first is TextInput.updateEditingState and
  // the second is TextInputClient.performAction.
  EXPECT_EQ(messages.size(), 2);

  // Editing state should have been updated.
  auto encoded_arguments = EncodedEditingState("\n", TextRange(1));
  auto update_state_message = codec.EncodeMethodCall(
      {"TextInputClient.updateEditingState", std::move(encoded_arguments)});

  EXPECT_TRUE(std::equal(update_state_message->begin(),
                         update_state_message->end(),
                         messages.front().begin()));

  // TextInputClient.performAction should have been called.
  auto arguments = std::make_unique<rapidjson::Document>(rapidjson::kArrayType);
  auto& allocator = arguments->GetAllocator();
  arguments->PushBack(kDefaultClientId, allocator);
  arguments->PushBack(
      rapidjson::Value("TextInputAction.newline", allocator).Move(), allocator);
  auto invoke_action_message = codec.EncodeMethodCall(
      {"TextInputClient.performAction", std::move(arguments)});

  EXPECT_TRUE(std::equal(invoke_action_message->begin(),
                         invoke_action_message->end(),
                         messages.back().begin()));
}

// Regression test for https://github.com/flutter/flutter/issues/125879.
TEST(TextInputPluginTest, VerifyInputActionSendDoesNotInsertNewLine) {
  std::vector<std::vector<uint8_t>> messages;

  TestBinaryMessenger messenger(
      [&messages](const std::string& channel, const uint8_t* message,
                  size_t message_size, BinaryReply reply) {
        int length = static_cast<int>(message_size);
        std::vector<uint8_t> last_message(length);
        memcpy(&last_message[0], &message[0], length * sizeof(uint8_t));
        messages.push_back(last_message);
      });
  BinaryReply reply_handler = [](const uint8_t* reply, size_t reply_size) {};

  MockTextInputPluginDelegate delegate;
  TextInputPlugin handler(&messenger, &delegate);

  auto& codec = JsonMethodCodec::GetInstance();

  // Call TextInput.setClient to initialize the TextInputModel.
  auto set_client_arguments =
      EncodedClientConfig("TextInputType.multiline", "TextInputAction.send");
  auto message = codec.EncodeMethodCall(
      {"TextInput.setClient", std::move(set_client_arguments)});
  messenger.SimulateEngineMessage("flutter/textinput", message->data(),
                                  message->size(), reply_handler);

  // Simulate a key down event for '\n'.
  handler.KeyboardHook(VK_RETURN, 100, WM_KEYDOWN, '\n', false, false);

  // Only a call to TextInputClient.performAction is expected.
  EXPECT_EQ(messages.size(), 1);

  // TextInputClient.performAction should have been called.
  auto arguments = std::make_unique<rapidjson::Document>(rapidjson::kArrayType);
  auto& allocator = arguments->GetAllocator();
  arguments->PushBack(kDefaultClientId, allocator);
  arguments->PushBack(
      rapidjson::Value("TextInputAction.send", allocator).Move(), allocator);
  auto invoke_action_message = codec.EncodeMethodCall(
      {"TextInputClient.performAction", std::move(arguments)});

  EXPECT_TRUE(std::equal(invoke_action_message->begin(),
                         invoke_action_message->end(),
                         messages.front().begin()));
}

TEST(TextInputPluginTest, TextEditingWorksWithDeltaModel) {
  auto handled_message = CreateResponse(true);
  auto unhandled_message = CreateResponse(false);
  int received_scancode = 0;

  TestBinaryMessenger messenger(
      [&received_scancode, &handled_message, &unhandled_message](
          const std::string& channel, const uint8_t* message,
          size_t message_size, BinaryReply reply) {});
  MockTextInputPluginDelegate delegate;

  int redispatch_scancode = 0;
  TextInputPlugin handler(&messenger, &delegate);

  auto args = std::make_unique<rapidjson::Document>(rapidjson::kArrayType);
  auto& allocator = args->GetAllocator();
  args->PushBack(123, allocator);  // client_id

  rapidjson::Value client_config(rapidjson::kObjectType);
  client_config.AddMember(kEnableDeltaModel, true, allocator);

  args->PushBack(client_config, allocator);
  auto encoded = JsonMethodCodec::GetInstance().EncodeMethodCall(
      MethodCall<rapidjson::Document>(kSetClientMethod, std::move(args)));

  EXPECT_TRUE(messenger.SimulateEngineMessage(
      kChannelName, encoded->data(), encoded->size(),
      [](const uint8_t* reply, size_t reply_size) {}));

  handler.KeyboardHook(VK_RETURN, 100, WM_KEYDOWN, '\n', false, false);
  handler.ComposeBeginHook();
  std::u16string text;
  text.push_back('\n');
  handler.ComposeChangeHook(text, 1);
  handler.ComposeEndHook();

  handler.KeyboardHook(0x4E, 100, WM_KEYDOWN, 'n', false, false);
  handler.ComposeBeginHook();
  std::u16string textN;
  text.push_back('n');
  handler.ComposeChangeHook(textN, 1);
  handler.KeyboardHook(0x49, 100, WM_KEYDOWN, 'i', false, false);
  std::u16string textNi;
  text.push_back('n');
  text.push_back('i');
  handler.ComposeChangeHook(textNi, 2);
  handler.KeyboardHook(VK_RETURN, 100, WM_KEYDOWN, '\n', false, false);
  std::u16string textChineseCharacter;
  text.push_back(u'\u4F60');
  handler.ComposeChangeHook(textChineseCharacter, 1);
  handler.ComposeCommitHook();
  handler.ComposeEndHook();

  // Passes if it did not crash
}

TEST(TextInputPluginTest, TransformCursorRect) {
  // A position of `EditableText`.
  double view_x = 100;
  double view_y = 200;

  // A position and size of marked text, in `EditableText` local coordinates.
  double ime_x = 3;
  double ime_y = 4;
  double ime_width = 50;
  double ime_height = 60;

  // Transformation matrix.
  std::array<std::array<double, 4>, 4> editabletext_transform = {
      1.0, 0.0, 0.0, view_x,  //
      0.0, 1.0, 0.0, view_y,  //
      0.0, 0.0, 0.0, 0.0,     //
      0.0, 0.0, 0.0, 1.0};

  TestBinaryMessenger messenger([](const std::string& channel,
                                   const uint8_t* message, size_t message_size,
                                   BinaryReply reply) {});
  BinaryReply reply_handler = [](const uint8_t* reply, size_t reply_size) {};

  MockTextInputPluginDelegate delegate;
  TextInputPlugin handler(&messenger, &delegate);

  auto& codec = JsonMethodCodec::GetInstance();

  EXPECT_CALL(delegate, OnCursorRectUpdated(Rect{{view_x, view_y}, {0, 0}}));

  {
    auto arguments =
        std::make_unique<rapidjson::Document>(rapidjson::kObjectType);
    auto& allocator = arguments->GetAllocator();

    rapidjson::Value transoform(rapidjson::kArrayType);
    for (int i = 0; i < 4 * 4; i++) {
      // Pack 2-dimensional array by column-major order.
      transoform.PushBack(editabletext_transform[i % 4][i / 4], allocator);
    }

    arguments->AddMember("transform", transoform, allocator);

    auto message = codec.EncodeMethodCall(
        {"TextInput.setEditableSizeAndTransform", std::move(arguments)});
    messenger.SimulateEngineMessage(kChannelName, message->data(),
                                    message->size(), reply_handler);
  }

  EXPECT_CALL(delegate,
              OnCursorRectUpdated(Rect{{view_x + ime_x, view_y + ime_y},
                                       {ime_width, ime_height}}));

  {
    auto arguments =
        std::make_unique<rapidjson::Document>(rapidjson::kObjectType);
    auto& allocator = arguments->GetAllocator();

    arguments->AddMember("x", ime_x, allocator);
    arguments->AddMember("y", ime_y, allocator);
    arguments->AddMember("width", ime_width, allocator);
    arguments->AddMember("height", ime_height, allocator);

    auto message = codec.EncodeMethodCall(
        {"TextInput.setMarkedTextRect", std::move(arguments)});
    messenger.SimulateEngineMessage(kChannelName, message->data(),
                                    message->size(), reply_handler);
  }
}

}  // namespace testing
}  // namespace flutter
