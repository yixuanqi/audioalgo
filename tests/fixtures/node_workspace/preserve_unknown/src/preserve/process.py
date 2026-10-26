def process(inputs, params, state):
    return {"main": inputs["audio_in"] * params["gain"]}
