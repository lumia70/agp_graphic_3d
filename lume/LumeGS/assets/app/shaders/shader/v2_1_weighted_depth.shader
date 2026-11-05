{
    "compatibility_info": {
        "version": "22.00",
        "type": "shader"
    },
    "shaders": [
        {
            "displayName": "V21 Weighted Depth",
            "vert": "gsshaders://shader/v2_1_splat.vert.spv",
            "frag": "gsshaders://shader/v2_1_weighted_depth.frag.spv",
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
                    "enableDepthTest": false,
                    "enableDepthWrite": false,
                    "enableDepthBoundsTest": false,
                    "enableStencilTest": false,
                    "depthCompareOp": "less"
                },
                "colorBlendState": {
                    "colorAttachments": [
                        {
                            "enableBlend": true,
                            "colorWriteMask": "r_bit|g_bit|b_bit|a_bit",
                            "srcColorBlendFactor": "one",
                            "dstColorBlendFactor": "one_minus_src_alpha",
                            "colorBlendOp": "add",
                            "srcAlphaBlendFactor": "one",
                            "dstAlphaBlendFactor": "one_minus_src_alpha",
                            "alphaBlendOp": "add"
                        },
                        {
                            "enableBlend": true,
                            "colorWriteMask": "r_bit|g_bit|b_bit|a_bit",
                            "srcColorBlendFactor": "one",
                            "dstColorBlendFactor": "one_minus_src_alpha",
                            "colorBlendOp": "add",
                            "srcAlphaBlendFactor": "one",
                            "dstAlphaBlendFactor": "one_minus_src_alpha",
                            "alphaBlendOp": "add"
                        }
                    ]
                }
            }
        }
    ]
}
