#include "engine/models/yue2/tokenizer_text.h"

#include "bpe-core.h"

#include <cstdint>
#include <string>
#include <vector>

namespace engine::models::yue2 {

// Decode byte-level BPE token ids back to text using the shared vendor
// vocabulary. Special tokens are skipped by default so the returned
// string is the plain planned score text (the <abc> window body).
std::string Yue2TextTokenizer::decode(
    const std::vector<int32_t> & token_ids,
    bool skip_special_tokens) const {
    return llama_tokenizer_vendor::decode_bpe(
        *vocab_, token_ids, skip_special_tokens);
}

}  // namespace engine::models::yue2