{
    "compatibility_info": {
        "version": "22.00",
        "type": "shader"
    },
    "shaders": [
        {
            "displayName": "Gaussian Splatting",
            "variantName": "TRANSLUCENT_FW",
            "renderSlot": "CORE3D_RS_DM_FW_TRANSLUCENT",
            "renderSlotDefaultShader": false,
            "vert": "gsshaders://shader/gs3d_gaussian_splatting.vert.spv",
            "frag": "gsshaders://shader/gs3d_gaussian_splatting.frag.spv",
            "vertexInputDeclaration": "gsvertexinputdeclarations://core3d_gs_dm_fw_depth.shadervid",
            "pipelineLayout": "gspipelinelayouts://gs3d_gaussian_splatting.shaderpl",
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
                    "depthCompareOp": "less_or_equal"
                },
                "colorBlendState": {
                    "colorAttachments": [
                    {
                        "enableBlend": true,
                        "colorWriteMask": "r_bit|g_bit|b_bit|a_bit",
                        "srcColorBlendFactor": "src_alpha",
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
