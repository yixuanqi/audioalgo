#include <audio_algorithms/SpeexDenoise.h>
#include <array>
#include <cmath>
#include <iostream>

int verify_speex_consumer() {
    audioalgo::SpeexDenoiseProcessor processor;
    if(processor.prepare({0},{16000,1})) return 50;
    std::array<float,480> input{},output{};
    for(std::size_t i=0;i<input.size();++i)input[i]=static_cast<float>(0.125*std::sin(i*0.03));
    const audioalgo::AudioChannelView in{16000,0,input,{0,480}};
    const audioalgo::MutableAudioChannelView out{16000,0,output,{0,480}};
    if(processor.process(in,out)) return 51;
    if(processor.process(in,out)) return 52;
    bool signal=false;
    for(const auto sample:output) {if(!std::isfinite(sample))return 53;signal|=std::abs(sample)>0.001f;}
    if(!signal)return 54;
    processor.reset();if(!processor.process(in,out))return 55;
    std::cout<<"independent SpeexDenoise consumer passed\n";
    return 0;
}
