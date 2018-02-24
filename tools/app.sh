#!/bin/bash

SECRET_ID=${TENCENT_CLOND_SECRET_ID}
SECRET_KEY=${TENCENT_CLOND_SECRET_KEY}
prodcut_id=iot-fdxt0fie
device_name=light001


# light shadow
# {"name":"colorful light","color":16777215,"brightness":100,"light_switch":false}
cmd_cfg="--product_id=$prodcut_id --device_name=$device_name -u $SECRET_ID -p $SECRET_KEY"
./bin/tc_iot_shadow_cli.py UpdateIotShadow $cmd_cfg --shadow='{"desired":{"name":"new light year","color":16777215,"brightness":89,"light_switch":true}}' | python -m json.tool
./bin/tc_iot_shadow_cli.py GetIotShadow $cmd_cfg |python -m json.tool