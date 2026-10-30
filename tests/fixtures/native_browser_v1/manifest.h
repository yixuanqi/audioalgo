#pragma once

namespace audioalgo::test_fixtures::native_browser_v1 {
inline constexpr char manifest[] = R"json({"schema_version":1,"plugin_id":"fixture.native_browser","version":"1.0.0","abi_version":1,"platform":"windows","architecture":"x64","library":"native_browser.dll","nodes":[{"type_id":"fixture.native_browser","version":"1.0.0","display_name":"Browser Native","ports":[{"id":"audio_in","direction":"input","type_id":"audio.pcm@1","required":true},{"id":"audio_out","direction":"output","type_id":"audio.pcm@1"}],"parameters":[],"sample_rates_hz":[48000],"channel_counts":[2],"supports_variable_block_size":true,"browser":{"schema_version":1,"category_id":"audio.dynamics","subcategory_id":"gain","tags":["gain","volume","增益"],"description":"调整音频电平"}}]})json";
}
