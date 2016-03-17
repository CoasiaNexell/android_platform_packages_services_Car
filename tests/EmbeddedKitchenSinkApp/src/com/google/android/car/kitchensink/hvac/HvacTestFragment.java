/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.google.android.car.kitchensink.hvac;

import android.car.CarNotConnectedException;
import android.car.hardware.hvac.CarHvacEvent;
import android.car.hardware.hvac.CarHvacManager.CarHvacBaseProperty;
import android.car.hardware.hvac.CarHvacManager.CarHvacBooleanValue;
import android.car.hardware.hvac.CarHvacManager.CarHvacFloatProperty;
import android.car.hardware.hvac.CarHvacManager.CarHvacFloatValue;
import android.car.hardware.hvac.CarHvacManager.CarHvacIntProperty;
import android.car.hardware.hvac.CarHvacManager.CarHvacIntValue;
import android.car.hardware.hvac.CarHvacManager;
import android.os.Bundle;
import android.support.v4.app.Fragment;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.TextView;
import android.widget.ToggleButton;

import com.android.car.vehiclenetwork.VehicleNetworkConsts.VehicleHvacFanDirection;
import com.android.car.vehiclenetwork.VehicleNetworkConsts.VehicleWindow;
import com.android.car.vehiclenetwork.VehicleNetworkConsts.VehicleZone;

import com.google.android.car.kitchensink.R;

import java.lang.Override;
import java.util.List;

public class HvacTestFragment extends Fragment {
    private final boolean DBG = true;
    private final String TAG = "HvacTestFragment";
    private RadioButton mRbFanPositionFace;
    private RadioButton mRbFanPositionFloor;
    private RadioButton mRbFanPositionFaceAndFloor;
    private ToggleButton mTbAc;
    private ToggleButton mTbDefrostFront;
    private ToggleButton mTbDefrostRear;
    private TextView mTvFanSpeed;
    private TextView mTvDTemp;
    private TextView mTvPTemp;
    private int mCurFanSpeed = 1;
    private float mCurDTemp = 23;
    private float mCurPTemp = 23;
    private CarHvacManager mCarHvacManager;
    private int mZoneForAcOn;
    private int mZoneForSetTempD;
    private int mZoneForSetTempP;
    private int mZoneForFanSpeed;
    private int mZoneForFanPosition;

    private final CarHvacManager.CarHvacEventListener mHvacListener =
            new CarHvacManager.CarHvacEventListener () {
                @Override
                public void onChangeEvent(final CarHvacManager.CarHvacBaseProperty value) {
                    int zones = value.getZones();
                    switch(value.getPropertyId()) {
                        case CarHvacManager.HVAC_ZONED_AC_ON:
                            mTbAc.setChecked(((CarHvacBooleanValue)value).getValue());
                            break;
                        case CarHvacManager.HVAC_ZONED_FAN_POSITION:
                            switch(((CarHvacIntValue)value).getValue()) {
                                case VehicleHvacFanDirection.VEHICLE_HVAC_FAN_DIRECTION_FACE:
                                    mRbFanPositionFace.setChecked(true);
                                    break;
                                case VehicleHvacFanDirection.VEHICLE_HVAC_FAN_DIRECTION_FLOOR:
                                    mRbFanPositionFloor.setChecked(true);
                                    break;
                                case VehicleHvacFanDirection.
                                        VEHICLE_HVAC_FAN_DIRECTION_FACE_AND_FLOOR:
                                    mRbFanPositionFaceAndFloor.setChecked(true);
                                    break;
                                default:
                                    Log.e(TAG, "Unknown fan position: " +
                                            ((CarHvacIntValue)value).getValue());
                                    break;
                            }
                            break;
                        case CarHvacManager.HVAC_ZONED_FAN_SPEED_SETPOINT:
                            if ((zones & mZoneForFanSpeed) != 0) {
                                mCurFanSpeed = ((CarHvacIntValue)value).getValue();
                                mTvFanSpeed.setText(String.valueOf(mCurFanSpeed));
                            }
                            break;
                        case CarHvacManager.HVAC_ZONED_TEMP_SETPOINT:
                            if ((zones & mZoneForSetTempD) != 0) {
                                mCurDTemp = ((CarHvacFloatValue)value).getValue();
                                mTvDTemp.setText(String.valueOf(mCurDTemp));
                            }
                            if ((zones & mZoneForSetTempP) != 0) {
                                mCurPTemp = ((CarHvacFloatValue)value).getValue();
                                mTvPTemp.setText(String.valueOf(mCurPTemp));
                            }
                            break;
                        case CarHvacManager.HVAC_WINDOW_DEFROSTER_ON:
                            if((zones & VehicleWindow.VEHICLE_WINDOW_FRONT_WINDSHIELD) ==
                                    VehicleWindow.VEHICLE_WINDOW_FRONT_WINDSHIELD) {
                                mTbDefrostFront.setChecked(((CarHvacBooleanValue)value).getValue());
                            }
                            if((zones & VehicleWindow.VEHICLE_WINDOW_REAR_WINDSHIELD) ==
                                    VehicleWindow.VEHICLE_WINDOW_REAR_WINDSHIELD) {
                                mTbDefrostRear.setChecked(((CarHvacBooleanValue)value).getValue());
                            }
                            break;
                        default:
                            Log.d(TAG, "onChangeEvent(): unknown property id = " + value
                                    .getPropertyId());
                    }
                }

                @Override
                public void onErrorEvent(final int propertyId, final int zone) {
                    Log.d(TAG, "Error:  propertyId=" + propertyId + "  zone=" + zone);
                }
            };

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        try {
            mCarHvacManager.registerListener(mHvacListener);
        } catch (CarNotConnectedException e) {
            Log.e(TAG, "Car is not connected!");
        }
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        mCarHvacManager.unregisterListener();
    }

    @Override
    public View onCreateView(LayoutInflater inflater, ViewGroup container, Bundle savedInstance) {
        View v = inflater.inflate(R.layout.hvac_test, container, false);

        List<CarHvacBaseProperty> props = mCarHvacManager.getPropertyList();

        for(CarHvacBaseProperty prop : props) {
            int propId = prop.getPropertyId();
            int type = prop.getType();

            if(DBG) {
                Log.d(TAG, prop.toString());
            }

            switch(propId) {
                case CarHvacManager.HVAC_ZONED_AC_ON:
                    configureAcOn(v, prop);
                    break;
                case CarHvacManager.HVAC_ZONED_FAN_POSITION:
                    configureFanPosition(v, prop);
                    break;
                case CarHvacManager.HVAC_ZONED_FAN_SPEED_SETPOINT:
                    configureFanSpeed(v, prop);
                    break;
                case CarHvacManager.HVAC_ZONED_TEMP_SETPOINT:
                    configureTempSetpoint(v, prop);
                    break;
                case CarHvacManager.HVAC_WINDOW_DEFROSTER_ON:
                    configureDefrosterOn(v, prop.getZones());
                    break;
                default:
                    Log.w(TAG, "propertyId " + propId + " is not handled");
                    break;
            }
        }

        mTvFanSpeed = (TextView) v.findViewById(R.id.tvFanSpeed);
        mTvFanSpeed.setText(String.valueOf(mCurFanSpeed));
        mTvDTemp = (TextView) v.findViewById(R.id.tvDTemp);
        mTvDTemp.setText(String.valueOf(mCurDTemp));
        mTvPTemp = (TextView) v.findViewById(R.id.tvPTemp);
        mTvPTemp.setText(String.valueOf(mCurPTemp));

        if(DBG) {
            Log.d(TAG, "Starting HvacTestFragment");
        }

        return v;
    }

    public void setHvacManager(CarHvacManager hvacManager) {
        Log.d(TAG, "setHvacManager()");
        mCarHvacManager = hvacManager;
    }

    private static int getFirstZone(int zones) {
        if (zones == 0) {
            return 0;
        }
        int flag = 0x1;
        for (int i = 0; i < 32; i++) {
            if ((flag & zones) != 0) {
                return flag;
            }
            flag <<= 1;
        }
        return 0;
    }

    private static int getNextZone(int zones, int startingZone) {
        int flag = startingZone << 1;
        while (flag != 0x80000000) {
            if ((flag & zones) != 0) {
                return flag;
            }
            flag <<= 1;
        }
        if ((flag & zones) != 0) {
            return flag;
        }
        return 0;
    }

    private void configureAcOn(View v, CarHvacBaseProperty prop) {
        mZoneForAcOn = getFirstZone(prop.getZones());
        mTbAc = (ToggleButton)v.findViewById(R.id.tbAc);
        mTbAc.setEnabled(true);
        mTbAc.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                // TODO handle zone properly
                mCarHvacManager.setBooleanProperty(CarHvacManager.HVAC_ZONED_AC_ON, mZoneForAcOn,
                        mTbAc.isChecked());
            }
        });
    }

    private void configureFanPosition(View v, CarHvacBaseProperty prop) {
        mZoneForFanPosition = getFirstZone(prop.getZones());
        RadioGroup rg = (RadioGroup)v.findViewById(R.id.rgFanPosition);
        rg.setOnCheckedChangeListener(new RadioGroup.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(RadioGroup group, int checkedId) {
                switch(checkedId) {
                    case R.id.rbPositionFace:
                        mCarHvacManager.setIntProperty(CarHvacManager.HVAC_ZONED_FAN_POSITION,
                                mZoneForFanPosition,
                                VehicleHvacFanDirection.VEHICLE_HVAC_FAN_DIRECTION_FACE);
                        break;
                    case R.id.rbPositionFloor:
                        mCarHvacManager.setIntProperty(CarHvacManager.HVAC_ZONED_FAN_POSITION,
                                mZoneForFanPosition,
                                VehicleHvacFanDirection.VEHICLE_HVAC_FAN_DIRECTION_FLOOR);
                        break;
                    case R.id.rbPositionFaceAndFloor:
                        mCarHvacManager.setIntProperty(CarHvacManager.HVAC_ZONED_FAN_POSITION,
                                mZoneForFanPosition,
                                VehicleHvacFanDirection.VEHICLE_HVAC_FAN_DIRECTION_FACE_AND_FLOOR);
                        break;
                }
            }
        });

        mRbFanPositionFace = (RadioButton)v.findViewById(R.id.rbPositionFace);
        mRbFanPositionFace.setClickable(true);
        mRbFanPositionFloor = (RadioButton)v.findViewById(R.id.rbPositionFloor);
        mRbFanPositionFaceAndFloor = (RadioButton)v.findViewById(R.id.rbPositionFaceAndFloor);
        mRbFanPositionFaceAndFloor.setClickable(true);
        mRbFanPositionFloor.setClickable(true);
    }

    private void configureFanSpeed(View v, CarHvacBaseProperty prop) {
        mZoneForFanSpeed = getFirstZone(prop.getZones());
        mCurFanSpeed = mCarHvacManager.getIntProperty(
                CarHvacManager.HVAC_ZONED_FAN_SPEED_SETPOINT,
                mZoneForFanSpeed);

        Button btnFanSpeedUp = (Button) v.findViewById(R.id.btnFanSpeedUp);
        btnFanSpeedUp.setEnabled(true);
        btnFanSpeedUp.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                if(mCurFanSpeed < 7) {
                    mCurFanSpeed++;
                    mTvFanSpeed.setText(String.valueOf(mCurFanSpeed));
                    mCarHvacManager.setIntProperty(CarHvacManager.HVAC_ZONED_FAN_SPEED_SETPOINT,
                            mZoneForFanSpeed, mCurFanSpeed);
                }
            }
        });

        Button btnFanSpeedDn = (Button) v.findViewById(R.id.btnFanSpeedDn);
        btnFanSpeedDn.setEnabled(true);
        btnFanSpeedDn.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                if (mCurFanSpeed > 1) {
                    mCurFanSpeed--;
                    mTvFanSpeed.setText(String.valueOf(mCurFanSpeed));
                    mCarHvacManager.setIntProperty(CarHvacManager.HVAC_ZONED_FAN_SPEED_SETPOINT,
                            mZoneForFanSpeed, mCurFanSpeed);
                }
            }
        });
    }

    private void configureTempSetpoint(View v, CarHvacBaseProperty prop) {
        mZoneForSetTempD = 0;
        if ((prop.getZones() & VehicleZone.VEHICLE_ZONE_ROW_1_LEFT) != 0) {
            mZoneForSetTempD = VehicleZone.VEHICLE_ZONE_ROW_1_LEFT;
        }
        mZoneForSetTempP = 0;
        if ((prop.getZones() & VehicleZone.VEHICLE_ZONE_ROW_1_RIGHT) != 0) {
            mZoneForSetTempP = VehicleZone.VEHICLE_ZONE_ROW_1_RIGHT;
        }
        if (mZoneForSetTempD == 0) {
            mZoneForSetTempD = getFirstZone(prop.getZones());
        }
        if (mZoneForSetTempP == 0) {
            mZoneForSetTempP = getNextZone(prop.getZones(), mZoneForSetTempD);
        }
        int numZones = CarHvacBaseProperty.getNumZones(prop.getZones());
        if (numZones < 2) {
            mZoneForSetTempP = 0;
        }
        Button btnDTempUp = (Button) v.findViewById(R.id.btnDTempUp);
        if (mZoneForSetTempD != 0) {
            mCurDTemp = mCarHvacManager.getFloatProperty(
                    CarHvacManager.HVAC_ZONED_TEMP_SETPOINT,
                    mZoneForSetTempD);
            btnDTempUp.setEnabled(true);
            btnDTempUp.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    if(mCurDTemp < 29.5) {
                        mCurDTemp += 0.5;
                        mTvDTemp.setText(String.valueOf(mCurDTemp));
                        mCarHvacManager.setFloatProperty(CarHvacManager.HVAC_ZONED_TEMP_SETPOINT,
                                mZoneForSetTempD, mCurDTemp);
                    }
                }
            });

            Button btnDTempDn = (Button) v.findViewById(R.id.btnDTempDn);
            btnDTempDn.setEnabled(true);
            btnDTempDn.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    if(mCurDTemp > 15.5) {
                        mCurDTemp -= 0.5;
                        mTvDTemp.setText(String.valueOf(mCurDTemp));
                        mCarHvacManager.setFloatProperty(CarHvacManager.HVAC_ZONED_TEMP_SETPOINT,
                                mZoneForSetTempD, mCurDTemp);
                    }
                }
            });
        } else {
            btnDTempUp.setEnabled(false);
        }

        Button btnPTempUp = (Button) v.findViewById(R.id.btnPTempUp);
        if (mZoneForSetTempP !=0 ) {
            mCurPTemp = mCarHvacManager.getFloatProperty(
                    CarHvacManager.HVAC_ZONED_TEMP_SETPOINT,
                    mZoneForSetTempP);
            btnPTempUp.setEnabled(true);
            btnPTempUp.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    if (mCurPTemp < 29.5) {
                        mCurPTemp += 0.5;
                        mTvPTemp.setText(String.valueOf(mCurPTemp));
                        mCarHvacManager.setFloatProperty(CarHvacManager.HVAC_ZONED_TEMP_SETPOINT,
                                mZoneForSetTempP, mCurPTemp);
                    }
                }
            });

            Button btnPTempDn = (Button) v.findViewById(R.id.btnPTempDn);
            btnPTempDn.setEnabled(true);
            btnPTempDn.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    if (mCurPTemp > 15.5) {
                        mCurPTemp -= 0.5;
                        mTvPTemp.setText(String.valueOf(mCurPTemp));
                        mCarHvacManager.setFloatProperty(CarHvacManager.HVAC_ZONED_TEMP_SETPOINT,
                                mZoneForSetTempP, mCurPTemp);
                    }
                }
            });
        } else {
            btnPTempUp.setEnabled(false);
        }
    }

    private void configureDefrosterOn(View v, int zone) {
        if((zone & VehicleWindow.VEHICLE_WINDOW_FRONT_WINDSHIELD) ==
                VehicleWindow.VEHICLE_WINDOW_FRONT_WINDSHIELD) {
            mTbDefrostFront = (ToggleButton)v.findViewById(R.id.tbDefrostFront);
            mTbDefrostFront.setEnabled(true);
            mTbDefrostFront.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    mCarHvacManager.setBooleanProperty(CarHvacManager.HVAC_WINDOW_DEFROSTER_ON,
                            VehicleWindow.VEHICLE_WINDOW_FRONT_WINDSHIELD,
                            mTbDefrostFront.isChecked());
                }
            });
        }
        if((zone & VehicleWindow.VEHICLE_WINDOW_REAR_WINDSHIELD) ==
                VehicleWindow.VEHICLE_WINDOW_REAR_WINDSHIELD) {
            mTbDefrostRear = (ToggleButton)v.findViewById(R.id.tbDefrostRear);
            mTbDefrostRear.setEnabled(true);
            mTbDefrostRear.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    mCarHvacManager.setBooleanProperty(CarHvacManager.HVAC_WINDOW_DEFROSTER_ON,
                            VehicleWindow.VEHICLE_WINDOW_REAR_WINDSHIELD,
                            mTbDefrostRear.isChecked());
                }
            });
        }
    }
}
