{
    "compatibility_info": {
        "version": "22.00",
        "type": "shader"
    },
    "shaders": [
        {
            "displayName": "V21 Opaque Discard",
            "vert": "gsshaders://shader/v2_1_splat.vert.spv",
            "frag": "gsshaders://shader/v2_1_opaque_disc.frag.spv",
            "vertexInputDeclaration": "gsvertexinputdeclarations://v2_1_no_vertex_input.shadervid",
            "pipelineLayout": "gspipelinelayouts://v2_1_graphics.shaderpl",
            "state": {
                "inputAssembly": {
                    "enablePrimitiveRestart": false,
                    "primitiveTopology": "triangle_strip"
                },
                "rasterizationState": {
                    "enableDepthClamp": false,
                    "enableDepthBias": false,
                    "enableRasterizerDiscard": false,
                    "polygonMode": "fill",
                    "cullModeFlags": "none",
                    "frontFace": "counter_clockwise"
                },
                "depthStencilState": {
                    "enableDepthTest": true,
                    "enableDepthWrite": true,
                    "enableDepthBoundsTest": false,
                    "enableStencilTest": false,
                    "depthCompareOp": "less"
                },
                "colorBlendState": {
                    "colorAttachments": [
                        {
                            "enableBlend": false,
                            "colorWriteMask": "r_bit",
                            "srcColorBlendFactor": "one",
                            "dstColorBlendFactor": "zero",
                            "colorBlendOp": "add",
                            "srcAlphaBlendFactor": "one",
                            "dstAlphaBlendFactor": "zero",
                            "alphaBlendOp": "add"
                        },
                        {
                            "enableBlend": false,
                            "colorWriteMask": "r_bit",
                            "srcColorBlendFactor": "one",
                            "dstColorBlendFactor": "zero",
                            "colorBlendOp": "add",
                            "srcAlphaBlendFactor": "one",
                            "dstAlphaBlendFactor": "zero",
                            "alphaBlendOp": "add"
                        }
                    ]
                }
            }
        }
    ]
}
