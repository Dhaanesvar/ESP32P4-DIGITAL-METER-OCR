/*
* SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
*
* SPDX-License-Identifier: ESPRESSIF MIT
*/

#include <string.h>
#include "esp_ipa.h"

typedef struct esp_video_ipa_index {
    const char *name;
    const esp_ipa_config_t *ipa_config;
} esp_video_ipa_index_t;

static const esp_ipa_ian_ct_basic_param_t s_ipa_ian_ct_OV2710_basic_param[] = {
    {
        .a0 = 1.433962,
        .a1 = 0.471698
    },
    {
        .a0 = 1.301887,
        .a1 = 0.566038
    },
    {
        .a0 = 1.196078,
        .a1 = 0.607843
    },
    {
        .a0 = 1.094340,
        .a1 = 0.660377
    },
    {
        .a0 = 1.037037,
        .a1 = 0.703704
    },
    {
        .a0 = 0.962963,
        .a1 = 0.722222
    },
    {
        .a0 = 0.924528,
        .a1 = 0.754717
    },
    {
        .a0 = 0.875000,
        .a1 = 0.785714
    },
    {
        .a0 = 0.846154,
        .a1 = 0.807692
    },
    {
        .a0 = 0.820000,
        .a1 = 0.820000
    },
    {
        .a0 = 0.773585,
        .a1 = 0.849057
    },
    {
        .a0 = 0.714286,
        .a1 = 0.857143
    },
    {
        .a0 = 0.679245,
        .a1 = 0.886792
    },
};

static const float s_esp_ipa_ian_ct_OV2710_g_a2[] = {
    -386.340321, 3152.046010, -8724.647577, 11215.405168, 
};

static const esp_ipa_ian_ct_config_t s_esp_ipa_ian_ct_OV2710_config = {
    .model = 2,
    .m_a0 = -0.035921,
    .m_a1 = -0.466694,
    .m_a2 = 1.220498,
    .f_n0 = 0.780000,
    .bp = s_ipa_ian_ct_OV2710_basic_param,
    .bp_nums = ARRAY_SIZE(s_ipa_ian_ct_OV2710_basic_param),
    .min_step = 110,
    .g_a0 = -0.332000,
    .g_a1 = -0.185800,
    .g_a2 = s_esp_ipa_ian_ct_OV2710_g_a2,
    .g_a2_nums = ARRAY_SIZE(s_esp_ipa_ian_ct_OV2710_g_a2)       
};

static const esp_ipa_ian_luma_ae_config_t s_esp_ipa_ian_luma_ae_OV2710_config = {                 
    .weight = {
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    },
};

static const float s_esp_ipa_ian_luma_env_speed_param_OV2710_config[] = {
    -0.005463, -0.010018, 0.000000, 0.033241, 0.085583, 0.136704, 0.160734, 0.148777, 0.148777, 0.160734, 0.136704, 0.085583, 0.033241, 0.000000, -0.010018, -0.005463, 
};

static const esp_ipa_ian_luma_env_config s_esp_ipa_ian_luma_env_OV2710_config = {
    .k = 250000.000000,
    .speed_param = s_esp_ipa_ian_luma_env_speed_param_OV2710_config,
    .speed_param_size = ARRAY_SIZE(s_esp_ipa_ian_luma_env_speed_param_OV2710_config),
    .weight = {
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    },
};

static const esp_ipa_ian_luma_config_t s_esp_ipa_ian_luma_OV2710_config = {
    .ae = &s_esp_ipa_ian_luma_ae_OV2710_config,
    .env = &s_esp_ipa_ian_luma_env_OV2710_config,
};

static const esp_ipa_ian_config_t s_ipa_ian_OV2710_config = {
    .ct = &s_esp_ipa_ian_ct_OV2710_config,
    .luma = &s_esp_ipa_ian_luma_OV2710_config,
};

static const esp_ipa_adn_bf_t s_ipa_adn_bf_OV2710_config[] = {
    {
        .gain = 1000,
        .bf = {
            .level = 3,
            .matrix = {
                {1, 3, 1},
                {3, 5, 3},
                {1, 3, 1}
            }
        }
    },
    {
        .gain = 17000,
        .bf = {
            .level = 3,
            .matrix = {
                {1, 2, 1},
                {2, 4, 2},
                {1, 2, 1}
            }
        }
    },
    {
        .gain = 49000,
        .bf = {
            .level = 4,
            .matrix = {
                {1, 2, 1},
                {2, 4, 2},
                {1, 2, 1}
            }
        }
    },
    {
        .gain = 560000,
        .bf = {
            .level = 6,
            .matrix = {
                {1, 3, 1},
                {3, 4, 3},
                {1, 3, 1}
            }
        }
    },
};

static const esp_ipa_adn_dm_t s_ipa_adn_dm_OV2710_config[] = {
    {
        .gain = 1000,
        .dm = {
            .gradient_ratio = 1.0000
        }
    },
};

static const esp_ipa_adn_config_t s_ipa_adn_OV2710_config = {
    .bf_table = s_ipa_adn_bf_OV2710_config,
    .bf_table_size = ARRAY_SIZE(s_ipa_adn_bf_OV2710_config),
    .dm_table = s_ipa_adn_dm_OV2710_config,
    .dm_table_size = ARRAY_SIZE(s_ipa_adn_dm_OV2710_config),
};

static const esp_ipa_aen_gamma_unit_t s_esp_ipa_aen_gamma_OV2710_table[] = {
    {
        .luma = 31.1000,
        .gamma_param = 0.5100,
    },
    {
        .luma = 75.1000,
        .gamma_param = 0.5300,
    },
    {
        .luma = 132.1000,
        .gamma_param = 0.5600,
    },
};

static const esp_ipa_aen_gamma_config_t s_ipa_aen_gamma_OV2710_config = {
    .model = 0,
    .use_gamma_param = true,
    .luma_env = "ae.luma.avg",
    .luma_min_step = 16.0000,
    .gamma_table = s_esp_ipa_aen_gamma_OV2710_table,
    .gamma_table_size = 3,
};

static const esp_ipa_aen_sharpen_t s_ipa_aen_sharpen_OV2710_config[] = {
    {
        .gain = 1000,
        .sharpen = {
            .h_thresh = 16,
            .l_thresh = 3,
            .h_coeff = 2.6500,
            .m_coeff = 2.9500,
            .matrix = {
                {1, 2, 1},
                {2, 2, 2},
                {1, 2, 1}
            }
        }
    },
    {
        .gain = 17000,
        .sharpen = {
            .h_thresh = 15,
            .l_thresh = 4,
            .h_coeff = 2.6500,
            .m_coeff = 2.4500,
            .matrix = {
                {1, 1, 1},
                {1, 1, 1},
                {1, 1, 1}
            }
        }
    },
    {
        .gain = 49000,
        .sharpen = {
            .h_thresh = 15,
            .l_thresh = 4,
            .h_coeff = 1.9500,
            .m_coeff = 2.1500,
            .matrix = {
                {1, 2, 1},
                {2, 1, 2},
                {1, 2, 1}
            }
        }
    },
    {
        .gain = 560000,
        .sharpen = {
            .h_thresh = 10,
            .l_thresh = 3,
            .h_coeff = 1.9500,
            .m_coeff = 1.6500,
            .matrix = {
                {1, 1, 1},
                {1, 2, 1},
                {1, 1, 1}
            }
        }
    },
};

static const esp_ipa_aen_con_t s_ipa_aen_con_OV2710_config[] = {
    {
        .gain = 1000,
        .contrast = 130
    },
    {
        .gain = 560000,
        .contrast = 128
    },
};

static const esp_ipa_aen_config_t s_ipa_aen_OV2710_config = {
    .gamma = &s_ipa_aen_gamma_OV2710_config,
    .sharpen_table = s_ipa_aen_sharpen_OV2710_config,
    .sharpen_table_size = ARRAY_SIZE(s_ipa_aen_sharpen_OV2710_config),
    .con_table = s_ipa_aen_con_OV2710_config,
    .con_table_size = ARRAY_SIZE(s_ipa_aen_con_OV2710_config),
};

static const esp_ipa_acc_sat_t s_ipa_acc_sat_OV2710_config[] = {
    {
        .color_temp = 0,
        .saturation = 128
    },
};

static const esp_ipa_acc_ccm_unit_t s_esp_ipa_acc_ccm_OV2710_table[] = {
    {
        .color_temp = 1200,
        .ccm = {
            .matrix = {
                { 1.0000, 0.0000, 0.0000 },
                { 0.0000, 1.0000, 0.0000 },
                { 0.0000, 0.0000, 1.0000 }
            }
        }
    },
    {
        .color_temp = 2314,
        .ccm = {
            .matrix = {
                { 2.2436, -0.3347, -0.9089 },
                { -0.4129, 1.3144, 0.0985 },
                { -0.5103, -2.4895, 3.9999 }
            }
        }
    },
    {
        .color_temp = 2882,
        .ccm = {
            .matrix = {
                { 2.3116, -0.4091, -0.9025 },
                { -0.3807, 1.5882, -0.2075 },
                { -0.3175, -1.8562, 3.1736 }
            }
        }
    },
    {
        .color_temp = 3372,
        .ccm = {
            .matrix = {
                { 2.0406, -0.7137, -0.3269 },
                { -0.3717, 1.4262, -0.0544 },
                { -0.4584, -1.3831, 2.8415 }
            }
        }
    },
    {
        .color_temp = 3880,
        .ccm = {
            .matrix = {
                { 2.0664, -0.7524, -0.3140 },
                { -0.3392, 1.4362, -0.0970 },
                { -0.3873, -1.1604, 2.5478 }
            }
        }
    },
    {
        .color_temp = 4310,
        .ccm = {
            .matrix = {
                { 2.0606, -0.7608, -0.2998 },
                { -0.3150, 1.4163, -0.1012 },
                { -0.3664, -0.9836, 2.3500 }
            }
        }
    },
    {
        .color_temp = 4679,
        .ccm = {
            .matrix = {
                { 2.1112, -0.8504, -0.2608 },
                { -0.2974, 1.4012, -0.1038 },
                { -0.3648, -0.9099, 2.2747 }
            }
        }
    },
    {
        .color_temp = 5097,
        .ccm = {
            .matrix = {
                { 2.1120, -0.8299, -0.2821 },
                { -0.2857, 1.3992, -0.1135 },
                { -0.3546, -0.8452, 2.1998 }
            }
        }
    },
    {
        .color_temp = 5470,
        .ccm = {
            .matrix = {
                { 2.1563, -0.8270, -0.3293 },
                { -0.1982, 1.3561, -0.1579 },
                { -0.2078, -0.9811, 2.1889 }
            }
        }
    },
    {
        .color_temp = 5780,
        .ccm = {
            .matrix = {
                { 2.0701, -0.7070, -0.3631 },
                { -0.2310, 1.4467, -0.2157 },
                { -0.2794, -0.7587, 2.0380 }
            }
        }
    },
    {
        .color_temp = 6086,
        .ccm = {
            .matrix = {
                { 2.0971, -0.7310, -0.3661 },
                { -0.2285, 1.4374, -0.2090 },
                { -0.2716, -0.7361, 2.0077 }
            }
        }
    },
    {
        .color_temp = 6670,
        .ccm = {
            .matrix = {
                { 2.0822, -0.5657, -0.5165 },
                { -0.2036, 1.4696, -0.2661 },
                { -0.1613, -0.8585, 2.0198 }
            }
        }
    },
    {
        .color_temp = 7200,
        .ccm = {
            .matrix = {
                { 2.0217, -0.4805, -0.5412 },
                { -0.2007, 1.5016, -0.3009 },
                { -0.1587, -0.7368, 1.8955 }
            }
        }
    },
    {
        .color_temp = 7847,
        .ccm = {
            .matrix = {
                { 2.0876, -0.5671, -0.5205 },
                { -0.1967, 1.4737, -0.2770 },
                { -0.1638, -0.7154, 1.8792 }
            }
        }
    },
    {
        .color_temp = 16000,
        .ccm = {
            .matrix = {
                { 1.0000, 0.0000, 0.0000 },
                { 0.0000, 1.0000, 0.0000 },
                { 0.0000, 0.0000, 1.0000 }
            }
        }
    },
};

static const esp_ipa_acc_ccm_config_t s_esp_ipa_acc_ccm_OV2710_config = {
    .model = 0,
    .luma_env = "ae.luma.avg",
    .luma_low_threshold = 28.0000,
    .luma_low_ccm = {
        .matrix = {
            { 1.0000, 0.0000, 0.0000 },
            { 0.0000, 1.0000, 0.0000 },
            { 0.0000, 0.0000, 1.0000 }
        }
    }
    ,
    .ccm_table = s_esp_ipa_acc_ccm_OV2710_table,
    .ccm_table_size = 15,
};

static const esp_ipa_acc_config_t s_ipa_acc_OV2710_config = {
    .sat_table = s_ipa_acc_sat_OV2710_config,
    .sat_table_size = ARRAY_SIZE(s_ipa_acc_sat_OV2710_config),
    .ccm = &s_esp_ipa_acc_ccm_OV2710_config,
};

static const esp_ipa_atc_luma_lut_t s_ipa_atc_luma_lut_OV2710_config[] = {
    {
        .luma = 250.000000,
        .ae_value = 48
    },
    {
        .luma = 500.000000,
        .ae_value = 60
    },
    {
        .luma = 750.000000,
        .ae_value = 75
    },
};

static const esp_ipa_atc_config_t s_ipa_atc_OV2710_config = {
    .model = 0,
    .init_value = 48,
    .delay_frames = 3,
    .luma_env = "env.luma.avg",
    .min_ae_value_step = 5,
    .luma_lut = s_ipa_atc_luma_lut_OV2710_config,
    .luma_lut_size = ARRAY_SIZE(s_ipa_atc_luma_lut_OV2710_config) 
};

static const char *s_ipa_OV2710_names[] = {
    "esp_ipa_ian",
    "esp_ipa_adn",
    "esp_ipa_aen",
    "esp_ipa_acc",
    "esp_ipa_atc",
};

static const esp_ipa_config_t s_ipa_OV2710_config = {
    .names = s_ipa_OV2710_names,
    .nums = ARRAY_SIZE(s_ipa_OV2710_names),
    .version = 1,
    .ian = &s_ipa_ian_OV2710_config,
    .adn = &s_ipa_adn_OV2710_config,
    .aen = &s_ipa_aen_OV2710_config,
    .acc = &s_ipa_acc_OV2710_config,
    .atc = &s_ipa_atc_OV2710_config,
};

static const esp_video_ipa_index_t s_video_ipa_configs[] = {
    {
        .name = "OV2710",
        .ipa_config = &s_ipa_OV2710_config
    },
};

const esp_ipa_config_t *esp_ipa_pipeline_get_config(const char *name)
{
    for (int i = 0; i < ARRAY_SIZE(s_video_ipa_configs); i++) {
        if (!strcmp(name, s_video_ipa_configs[i].name)) {
            return s_video_ipa_configs[i].ipa_config;
        }
    }
    return NULL;
}

/* Json file: /home/dhaanesvar/Desktop/ESP32P4 EYE/managed_components/espressif__esp_cam_sensor/sensors/ov2710/cfg/ov2710_default.json */

