/*
 *  Copyright (c) 2018-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree.
 */

#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>

#include <fizz/server/CookieTypes.h>

#include <fizz/crypto/test/TestUtil.h>
#include <fizz/protocol/test/TestMessages.h>

using namespace fizz::test;

static constexpr folly::StringPiece secret{
    "c44ed3fb98c179579036d201735f43af20a856470b9c527fe07f01f3a2a0bde9"};

static constexpr folly::StringPiece retry{
    "1603030099020000950303cf21ad74e59a6111be1d8c021e65b891c2a211167abb8c5e079e09e2c8a8339c00130100006d002b00020304002c0063006144444444444444444444444444444444444444444444444444444444444444440000000099d67e4a6c0776e1b52119d2d06dc27c9d40d131856e077b6ef9901c652910a92a703a91fc04d90e1700ce9d4247fd0bf575aed4482be227d61a7b725d"};

static constexpr folly::StringPiece retryGroup{
    "16030300a10200009d0303cf21ad74e59a6111be1d8c021e65b891c2a211167abb8c5e079e09e2c8a8339c001301000075002b00020304003300020017002c0065006344444444444444444444444444444444444444444444444444444444444444440000000099d67e4a6d07414a08e5e0be2f66b9982a741909c185f48630afa8abd44c5dab460001c8948e4cdd0b74af9a53ed5665c295eed49d1862d4967c0ed002780b"};

static constexpr folly::StringPiece testCookie{
    "444444444444444444444444444444444444444444444444444444444444444400000000e5c57e4a6c07762b1c4fcbc41e05abbc7f964506ce11cec423060f95f3a263df93e8e573f6abcf0e1700ce9d42df8b8fdf63535b8e3c6bed8f919a4ef5"};

static constexpr folly::StringPiece testCookieGroup{
    "444444444444444444444444444444444444444444444444444444444444444400000000e5c57e4a6d07414a082f49d0fd7077f043b4fbdf55b2bff9f910e5544bc5cb203576b8504b6c46721d74af9a53ed5602983e52a143aeb7854637e22261263c"};

namespace fizz {
namespace server {
namespace test {

class AeadCookieCipherTest : public Test {
 public:
  void SetUp() override {
    context_ = std::make_shared<FizzServerContext>();
    context_->setSupportedVersions({ProtocolVersion::tls_1_3});
    cipher_ = std::make_shared<AES128CookieCipher>();
    cipher_->setContext(context_.get());

    auto s = toIOBuf(secret);
    std::vector<folly::ByteRange> cookieSecrets{{s->coalesce()}};
    EXPECT_TRUE(cipher_->setCookieSecrets(std::move(cookieSecrets)));
  }

 protected:
  Buf getClientHello(Buf cookie) {
    auto chlo = TestMessages::clientHello();

    if (cookie) {
      Cookie c;
      c.cookie = std::move(cookie);
      chlo.extensions.push_back(encodeExtension(std::move(c)));
    }

    return PlaintextWriteRecordLayer()
        .writeInitialClientHello(encodeHandshake(std::move(chlo)))
        .data;
  }

  std::shared_ptr<FizzServerContext> context_;
  std::shared_ptr<AES128CookieCipher> cipher_;
};

TEST_F(AeadCookieCipherTest, TestGetRetry) {
  useMockRandom();
  auto res = cipher_->getTokenOrRetry(
      getClientHello(nullptr), folly::IOBuf::copyBuffer("test"));
  auto msg = std::move(boost::get<StatelessHelloRetryRequest>(res));
  EXPECT_EQ(hexlify(msg.data->coalesce()), retry);
}

TEST_F(AeadCookieCipherTest, TestGetRetryGroup) {
  useMockRandom();
  context_->setSupportedGroups({NamedGroup::secp256r1});
  auto res = cipher_->getTokenOrRetry(
      getClientHello(nullptr), folly::IOBuf::copyBuffer("test"));
  auto msg = std::move(boost::get<StatelessHelloRetryRequest>(res));
  EXPECT_EQ(hexlify(msg.data->coalesce()), retryGroup);
}

TEST_F(AeadCookieCipherTest, TestGetToken) {
  auto res = cipher_->getTokenOrRetry(
      getClientHello(toIOBuf(testCookie)), folly::IOBuf::copyBuffer("xx"));
  auto token = std::move(boost::get<AppToken>(res));
  EXPECT_TRUE(
      folly::IOBufEqualTo()(token.token, folly::IOBuf::copyBuffer("test")));
}

TEST_F(AeadCookieCipherTest, TestGetJunk) {
  EXPECT_THROW(
      cipher_->getTokenOrRetry(
          folly::IOBuf::copyBuffer("junk"), folly::IOBuf::copyBuffer("test")),
      std::runtime_error);
}

TEST_F(AeadCookieCipherTest, TestGetPartial) {
  auto trimmed = getClientHello(toIOBuf(testCookie));
  trimmed->coalesce();
  trimmed->trimEnd(1);
  EXPECT_THROW(
      cipher_->getTokenOrRetry(
          std::move(trimmed), folly::IOBuf::copyBuffer("test")),
      std::runtime_error);
}

TEST_F(AeadCookieCipherTest, TestDecrypt) {
  auto state = cipher_->decrypt(toIOBuf(testCookie));
  EXPECT_TRUE(state.has_value());
  EXPECT_TRUE(
      folly::IOBufEqualTo()(state->appToken, folly::IOBuf::copyBuffer("test")));
  EXPECT_FALSE(state->group.has_value());
}

TEST_F(AeadCookieCipherTest, TestDecryptGroup) {
  auto state = cipher_->decrypt(toIOBuf(testCookieGroup));
  EXPECT_TRUE(state.has_value());
  EXPECT_TRUE(
      folly::IOBufEqualTo()(state->appToken, folly::IOBuf::copyBuffer("test")));
  EXPECT_EQ(*state->group, NamedGroup::secp256r1);
}

TEST_F(AeadCookieCipherTest, TestDecryptMultipleSecrets) {
  auto s = toIOBuf(secret);
  auto s1 = RandomGenerator<32>().generateRandom();
  auto s2 = RandomGenerator<32>().generateRandom();
  std::vector<folly::ByteRange> cookieSecrets{
      {folly::range(s1), folly::range(s2), s->coalesce()}};
  EXPECT_TRUE(cipher_->setCookieSecrets(std::move(cookieSecrets)));

  auto state = cipher_->decrypt(toIOBuf(testCookie));
  EXPECT_TRUE(state.has_value());
  EXPECT_TRUE(
      folly::IOBufEqualTo()(state->appToken, folly::IOBuf::copyBuffer("test")));
  EXPECT_FALSE(state->group.has_value());
}

TEST_F(AeadCookieCipherTest, TestDecryptFailed) {
  auto s1 = RandomGenerator<32>().generateRandom();
  auto s2 = RandomGenerator<32>().generateRandom();
  std::vector<folly::ByteRange> cookieSecrets{
      {folly::range(s1), folly::range(s2)}};
  EXPECT_TRUE(cipher_->setCookieSecrets(std::move(cookieSecrets)));

  auto state = cipher_->decrypt(toIOBuf(testCookie));
  EXPECT_FALSE(state.has_value());
}
} // namespace test
} // namespace server
} // namespace fizz
