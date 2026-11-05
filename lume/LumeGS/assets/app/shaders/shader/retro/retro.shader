{
    "compatibility_info" : { "version" : "22.00", "type" : "shader" },
    "vert": "rendershaders://shader/fullscreen_triangle.vert.spv",
    "frag": "gsshaders://shader/retro/retro.frag.spv",
    "state": {
        "colorBlendState": {
            "colorAttachments": [
                {
                    "colorWriteMask": "r_bit|g_bit|b_bit|a_bit"
                }
            ]
        }
    },
    "materialMetadata": [
        {
            "name": "ShaderPipelineBinder",
            "customProperties": [
                {
                    "set": 0,
                    "binding": 0,
                    "data": [
                        {
                            "name": "params",
                            "displayName": "Time, NA",
                            "type": "vec2",
                            "value": [0.0, 0.0]
                        },
                        {
                            "name": "resolution",
                            "displayName": "Resolution.xy",
                            "type": "vec2",
                            "value": [1.0, 1.0]
                        }
                    ]
                }
            ],
            "bindingProperties": [
                {
                    "data": [
                        {
                            "name": "uImage",
                            "displayName": "Background Image",
                            "type": "image",
                            "set": 0,
                            "binding": 1
                        },
                        {
                            "name": "uSampler",
                            "displayName": "Sampler",
                            "type": "sampler",
                            "set": 0,
                            "binding": 2
                        }
                    ]
                }
            ]
        }
    ]
}
