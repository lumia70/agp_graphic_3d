{
    "compatibility_info": {
        "version": "22.00",
        "type": "shader"
    },
    "shaders": [
        {
            "displayName": "Gaussian Splatting Depth",
            "vert": "gsshaders://shader/gs3d_gaussian_splatting_render_depth.vert.spv",
            "frag": "gsshaders://shader/gs3d_gaussian_splatting_render_depth.frag.spv",
            "vertexInputDeclaration": "gsvertexinputdeclarations://core3d_gs_dm_fw.shadervid",
            "pipelineLayout": "gspipelinelayouts://gs3d_gaussian_splatting_render_depth.shaderpl",
            "state": {
                "inputAssembly": {
                    "enablePrimitiveRestart": false,
                    "primitiveTopology": "triangle_fan"
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
                    "depthCompareOp": "less_or_equal"
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
                            "colorWriteMask": "r_bit|g_bit",
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
