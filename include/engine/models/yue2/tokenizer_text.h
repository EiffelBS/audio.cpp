#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace llama_tokenizer_vendor {
struct BpeVocabulary;
}  // namespace llama_tokenizer_vendor

namespace engine::models::yue2 {

class Yue2TextTokenizer {
public:
    explicit Yue2TextTokenizer(const std::filesystem::path & vocab_path);

    std::vector<int32_t> encode(const std::string & text) const;

    /** Decode token ids back to text (byte-level BPE, special tokens
        skipped by default). Used to surface the model-planned ABC score. */
    std::string decode(const std::vector<int32_t> & token_ids,
                       bool skip_special_tokens = true) const;

private:
    std::shared_ptr<llama_tokenizer_vendor::BpeVocabulary> vocab_;
};

}  // namespace engine::models::yue2
