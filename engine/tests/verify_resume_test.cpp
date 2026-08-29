#include "../src/cli/verify_resume.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using getnative::cli::remaining_verify_frames;
using getnative::media::FrameIdentity;

void expect(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string{message});
}

[[nodiscard]] FrameIdentity identity_at(std::uint64_t frame_index) {
    FrameIdentity identity;
    identity.frame_index = frame_index;
    return identity;
}

void test_resume_skips_finished_seqs() {
    const std::vector<FrameIdentity> selected{
        identity_at(10), identity_at(11), identity_at(12),
        identity_at(13), identity_at(14), identity_at(15),
    };
    const std::vector<char> finished{1, 0, 1, 0, 0, 1};
    const auto remaining = remaining_verify_frames(selected, finished);
    expect(remaining.original_seqs.size() == 3, "three unfinished seqs");
    expect(remaining.original_seqs[0] == 1 && remaining.identities[0].frame_index == 11,
           "first remaining is seq 1");
    expect(remaining.original_seqs[1] == 3 && remaining.identities[1].frame_index == 13,
           "second remaining is seq 3");
    expect(remaining.original_seqs[2] == 4 && remaining.identities[2].frame_index == 14,
           "third remaining is seq 4");
}

void test_resume_all_or_none() {
    const std::vector<FrameIdentity> selected{identity_at(0), identity_at(1)};
    const auto none_finished = remaining_verify_frames(selected, std::vector<char>{0, 0});
    expect(none_finished.original_seqs.size() == 2
               && none_finished.original_seqs[0] == 0
               && none_finished.original_seqs[1] == 1,
           "unfinished selection keeps original seqs");
    const auto all_finished = remaining_verify_frames(selected, std::vector<char>{1, 1});
    expect(all_finished.identities.empty() && all_finished.original_seqs.empty(),
           "fully finished selection has nothing to resume");
}

} // namespace

int main() {
    try {
        test_resume_skips_finished_seqs();
        test_resume_all_or_none();
    } catch (const std::exception &error) {
        std::cerr << "verify_resume_test: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
