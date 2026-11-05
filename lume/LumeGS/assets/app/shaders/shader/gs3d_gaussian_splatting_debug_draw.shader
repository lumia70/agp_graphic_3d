{
    "compatibility_info": {
        "version": "22.00",
        "type": "shader"
    },
    "shaders": [
        {
            "displayName": "Gaussian Splatting",
            "vert": "gsshaders://shader/gs3d_gaussian_splatting_debug_draw.vert.spv",
            "frag": "gsshaders://shader/gs3d_gaussian_splatting_debug_draw.frag.spv",
            "vertexInputDeclaration": "gsvertexinputdeclarations://core3d_gs_dm_fw.shadervid",
            "pipelineLayout": "gspipelinelayouts://gs3d_gaussian_splatting_debug_draw.shaderpl",
            "state": {
                "inputAssembly": {
                    "enablePrimitiveRestart": false,
                    "primitiveTopology": "line_list"
                },
                "rasterizationState": {
                    "enableDepthClamp": false,
                    "enableDepthBias": false,
                    "enableRasterizerDiscard": false,
                    "polygonMode": "line",
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
                        "dstColorBlendFactor": "one",
                        "colorBlendOp": "add",
                        "srcAlphaBlendFactor": "one",
                        "dstAlphaBlendFactor": "one",
                        "alphaBlendOp": "add"
                    }
                    ]
                }
            }
        }
    ]
}
