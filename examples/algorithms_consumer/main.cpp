#include <audio_algorithms/Gain.h>
#include <audio_algorithms/ChannelSwap.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <variant>
#include <vector>

int main() {
    audioalgo::AudioBuffer input(48000, {"L"}, 2);
    input.mutable_channel(0)[0] = 0.25f;
    input.mutable_channel(0)[1] = -0.5f;
    const auto original = std::vector<float>(input.channel(0).begin(), input.channel(0).end());

    const auto outcome = audioalgo::apply_gain_db(input, 6.020599913279624);
    const auto* output = std::get_if<audioalgo::AudioBuffer>(&outcome);
    if (output == nullptr)
        return 1;
    const auto samples = output->channel(0);
    if (std::abs(samples[0] - 0.5f) > 2.0e-6f || std::abs(samples[1] + 1.0f) > 2.0e-6f)
        return 2;
    if (!std::equal(input.channel(0).begin(), input.channel(0).end(), original.begin(), original.end()))
        return 3;

    std::cout << "independent Gain consumer passed\n";
    audioalgo::AudioBuffer stereo(16000, {"L", "R"}, 3);
    stereo.mutable_channel(0)[0] = 0.125f;
    stereo.mutable_channel(1)[0] = -0.75f;
    const auto swapped = audioalgo::apply_channel_swap(stereo);
    const auto* swap_output = std::get_if<audioalgo::AudioBuffer>(&swapped);
    if (swap_output == nullptr || swap_output->channel(0)[0] != -0.75f || swap_output->channel(1)[0] != 0.125f ||
        swap_output->channel_ids() != stereo.channel_ids() || swap_output->frame_count() != 3)
        return 4;
    if (stereo.channel(0)[0] != 0.125f || stereo.channel(1)[0] != -0.75f)
        return 5;
    std::cout << "independent ChannelSwap consumer passed\n";
    return 0;
}
