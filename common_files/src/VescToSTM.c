#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "VescToSTM.h"
#include "mc_interface.h"
#ifndef M4F
#include "stm32f1xx_hal.h"
#else
#include "stm32f3xx_hal.h"
#endif
#include "crc.h"
#include "defines.h"
#include "drive_parameters.h"
#include "mc_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include "conf_general.h"
#include "utils.h"
#include "product.h"
#include "current_sense.h"
#include "app.h"
#include "ninebot.h"
#include "VescCommand.h"

static float tacho_scale;
stm_state VescToSTM_mode;

float adc_1;
float adc_2;

float motor_voltage; //calculates motor voltage from ERPM and flux linkage


typedef struct {
	uint16_t battery_cut_start;
	uint16_t battery_cut_end;
	uint16_t temp_cut_start;
	uint16_t temp_cut_end;
	int32_t minimum_current;
}fp_struct;

fp_struct fp;

volatile bool pwm_force; //State of PWM force on

/**
 * Calculates integer torque from mA.
 *
 * @param curr_ma
 * Setpoint in mA
 *
 * @return
 * Speed, in m/s
 */
int32_t current_to_torque(int32_t curr_ma){
	float ret = curr_ma * CURRENT_FACTOR_mA;
	return ret;
}

/**
 * Sets the minimum current for PWM off feature
 *
 * @param currrent
 * Limit in Ampere
 */
void VescToSTM_set_minimum_current(float current){
	fp.minimum_current = current_to_torque(current*1000);
}

void VescToSTM_set_battery_cut(float start, float end){
	fp.battery_cut_start = start * BATTERY_VOLTAGE_GAIN;
	fp.battery_cut_end = end * BATTERY_VOLTAGE_GAIN;
}

uint16_t VescToSTM_temp_to_int(float temp){
	float ret = ((V0_V + (dV_dT * (T0_C-temp)))*INT_SUPPLY_VOLTAGE);
	return ret;
}

void VescToSTM_set_temp_cut(float start, float end){
	utils_truncate_number(&end, 0, 75); //cannot measure more than 75°C for now (Hardware?)
	utils_truncate_number(&start, 0, 75); //cannot measure more than 75°C for now (Hardware?)
	TempSensorParamsM1.hOverTempThreshold      = VescToSTM_temp_to_int(end);
	fp.temp_cut_end							   = VescToSTM_temp_to_int(end);
	fp.temp_cut_start 						   = VescToSTM_temp_to_int(start);
	TempSensorParamsM1.hOverTempDeactThreshold = VescToSTM_temp_to_int(end-OV_TEMPERATURE_HYSTERESIS_C);
}



/**
 * Force PWM on
 */
void VescToSTM_pwm_force(bool force, bool update){
	pwm_force = force;
	if(update==true){
		VescToSTM_pwm_start();
	}else{
		VescToSTM_pwm_stop();
	}
}

bool VescToSTM_overspeed(){
	if(MCI_GetAvrgMecSpeedUnit(pMCI[M1]) > MCI_GetLastRampFinalSpeed(pMCI[M1])){
		return true;
	}else{
		return false;
	}
}

int16_t VescToSTM_Iq_lim_hook(int16_t iq){

	//Do temperature Iq limit
	uint16_t temp = NTC_GetAvTemp_d(pMCT[M1]->pTemperatureSensor);
	if(temp < fp.temp_cut_start){
		app_adc_set_mode(M365_MODE_TEMP);
		iq = utils_map_int(temp, fp.temp_cut_start, fp.battery_cut_end, 0, iq);
	}else{
		app_adc_clear_mode(M365_MODE_TEMP);
	}

	//Do battery Iq limit
	if(VescToSTM_mode != STM_STATE_BRAKE){
		uint16_t Vin = VBS_GetAvBusVoltage_d(pMCT[M1]->pBusVoltageSensor);
		if(Vin < fp.battery_cut_start){
			if(Vin < fp.battery_cut_end){
				iq=0;
			}else{
				iq = utils_map_int(Vin, fp.battery_cut_start, fp.battery_cut_end, iq, 0);
			}
		}
	}

	//Do PWM off feature
	if(fp.minimum_current>0 && pMCI[M1]->pSTM->hFaultOccurred == 0){
		if(pwm_force == false && abs(iq) <= fp.minimum_current && abs(FW_M1.AvVolt_qd.q) < MIN_DUTY_PWM){
			if(VescToSTM_overspeed()==false){
				VescToSTM_pwm_stop();
			}
		}else{
			VescToSTM_pwm_start();  //Function checks if PWM off otherwise it does nothing
		}
	}
	return iq;
}

int32_t VescToSTM_rpm_to_speed(int32_t rpm){
	int32_t speed = ((rpm*SPEED_UNIT*mc_conf.si_motor_poles)/_RPM);
	return speed;
}

int32_t VescToSTM_speed_to_rpm(int32_t speed){
	int32_t rpm = (speed*60/10)/mc_conf.si_motor_poles;
	return rpm;
}

int32_t VescToSTM_speed_to_erpm(int32_t speed){
	int32_t erpm = speed*60/10;
	return erpm;
}

int32_t VescToSTM_erpm_to_speed(int32_t erpm){
	int32_t speed = ((erpm*SPEED_UNIT)/_RPM);
	return speed;
}

static int16_t erpm_to_int16(int32_t erpm){
	int32_t speed = VescToSTM_erpm_to_speed(erpm);
	int32_t out = ((int32_t)HALL_M1._Super.DPPConvFactor * speed) / ((int32_t) SPEED_UNIT * (int32_t)HALL_M1._Super.hMeasurementFrequency);
	return out;
}


static int last_y;
void VescToStm_nunchuk_update_erpm(){
	if(VescToSTM_mode == STM_STATE_NUNCHUCK){
		if(last_y > 126){
			VescToSTM_set_current_rel_int(utils_map_int(last_y, 127, 255, 0, 32768));
		}else{
			VescToSTM_set_brake_rel_int(utils_map(last_y, 0, 126, 32768, 0));
		}
	}
}

void VescToStm_nunchuk_update_output(chuck_data * chuck_d){
	VescToSTM_mode = STM_STATE_NUNCHUCK;
	VescToSTM_timeout_reset();
	if(chuck_d->js_y != last_y){
		last_y = chuck_d->js_y;
		if(appconf.app_chuk_conf.ctrl_type == CHUK_CTRL_TYPE_NONE) return;
		float out_val = (chuck_d->js_y - 128.0) / 128.0;
		utils_deadband(&out_val, appconf.app_chuk_conf.hyst, 1.0);
		out_val = utils_throttle_curve(out_val, appconf.app_chuk_conf.throttle_exp, appconf.app_chuk_conf.throttle_exp_brake, appconf.app_chuk_conf.throttle_exp_mode);
		if(out_val > 0.0){
			VescToSTM_set_current_rel(out_val);
		}else{
			if((chuck_d->bt_c && appconf.app_chuk_conf.ctrl_type == CHUK_CTRL_TYPE_CURRENT) || appconf.app_chuk_conf.ctrl_type == CHUK_CTRL_TYPE_CURRENT_BIDIRECTIONAL){
				VescToSTM_set_current_rel(out_val);
			}else{
				VescToSTM_set_brake_current_rel(out_val);
			}
		}
	}
}

float VescToStm_nunchuk_get_decoded_chuk(void){
	return ((float)last_y - 128.0) / 128.0;
}


void VescToSTM_set_open_loop(bool enabled, int16_t init_angle, int16_t erpm){
	if(enabled){
		VescToSTM_mode = STM_STATE_OPENLOOP;
		SpeednTorqCtrlM1.SPD->open_angle = init_angle;
		SpeednTorqCtrlM1.SPD->open_loop = true;
		SpeednTorqCtrlM1.SPD->open_speed = erpm_to_int16(erpm);
	}else{
		SpeednTorqCtrlM1.SPD->open_loop = false;
		SpeednTorqCtrlM1.SPD->open_speed = 0;
	}
}

void VescToSTM_ramp_current(float iq, float id){ //in Amps
	iq*=1000;
	id*=1000;
	qd_t currComp;
	for (int i = 0;i < 1000;i++) {
		currComp.q = current_to_torque((float)i * iq / 1000.0);
		currComp.d = current_to_torque((float)i * id / 1000.0);

		MCI_SetCurrentReferences(pMCI[M1],currComp);
		vTaskDelay(MS_TO_TICKS(1));
		VescToSTM_timeout_reset();
	}
}

void VescToSTM_set_current(float iq, float id){ //in Amps
	iq*=1000;
	id*=1000;
	qd_t currComp;
	currComp.q = current_to_torque(iq);
	currComp.d = current_to_torque(id);
	MCI_SetCurrentReferences(pMCI[M1],currComp);
}

void VescToSTM_set_open_loop_erpm(int16_t erpm){
	SpeednTorqCtrlM1.SPD->open_speed = erpm_to_int16(erpm);
}


const uint8_t test[12] = {1,2,3,4,5,6,7,8,9,10,11,12};
uint8_t VescToSTM_get_uid(uint8_t * ptr, uint8_t size){
	if(size>12) size=12;
	memcpy(ptr, test, size);
	return size;
}

float VescToSTM_get_ADC1(){
	return adc_1;
}
void VescToSTM_set_ADC1(float val){
	adc_1 = val;
}
float VescToSTM_get_ADC2(){
	return adc_2;
}
void VescToSTM_set_ADC2(float val){
	adc_2 = val;
}

float VescToSTM_get_pid_pos_now(){
	return 360.0 / 65536.0 * (float)SpeednTorqCtrlM1.SPD->hElAngle;
}

uint32_t last_reset=0;
bool timeout_enable = true;
bool timeout_triggerd = false;

bool VescToSTM_get_timeout_state(){
	return timeout_triggerd;
}


void VescToSTM_timeout_reset(){
	last_reset = xTaskGetTickCount();
	timeout_triggerd = false;
};
void VescToSTM_handle_timeout(){
	if(!timeout_enable) {
		VescToSTM_timeout_reset();
	}
	if(appconf.timeout_msec){
		if((xTaskGetTickCount() - last_reset) > (appconf.timeout_msec*2)){
			VescToSTM_set_brake(appconf.timeout_brake_current*1000);
			app_adc_stop_output();
			last_reset = xTaskGetTickCount();
			timeout_triggerd = true;
		}
	}
};
void VescToSTM_enable_timeout(bool enbale){
	timeout_enable = enbale;
}


void VescToSTM_pwm_stop(void){
	if(PWM_Handle_M1._Super.PWM_off == false){
		PWMC_SwitchOffPWM(&PWM_Handle_M1._Super);
		PIDIdHandle_M1.wIntegralTerm = 0;
		PIDIqHandle_M1.wIntegralTerm = 0;
		FOCVars[M1].Vqd.d=0;
		FOCVars[M1].Vqd.q=0;
		FW_Clear(&FW_M1);
	}else{
		int32_t erpm = VescToSTM_get_erpm_fast();
		float rad_s = erpm * ((2.0 * M_PI) / 60.0);
		motor_voltage = rad_s * mc_conf.foc_motor_flux_linkage;
	}
}

void VescToSTM_pwm_start(void){
	if(PWM_Handle_M1._Super.PWM_off){
		float fVd = 32768.0 / VescToSTM_get_bus_voltage() * motor_voltage;
		utils_truncate_number(&fVd, -32767, 32767);
		PIDIqHandle_M1.wIntegralTerm = fVd * TF_KIDIV;
		PIDIdHandle_M1.wIntegralTerm = -PIDIqHandle_M1.wIntegralTerm/4;
		PWMC_SwitchOnPWM(&PWM_Handle_M1._Super);
	}
}

void VescToSTM_update_torque(int32_t q, int32_t min_erpm, int32_t max_erpm){
	q *= DIR_MUL;
	if(q >= 0){
		SpeednTorqCtrlM1.PISpeed->wUpperIntegralLimit = q * SP_KIDIV;
		SpeednTorqCtrlM1.PISpeed->wLowerIntegralLimit = mc_conf.s_pid_allow_braking ? -q * SP_KIDIV : 0;
		SpeednTorqCtrlM1.PISpeed->hUpperOutputLimit = q;
		SpeednTorqCtrlM1.PISpeed->hLowerOutputLimit = mc_conf.s_pid_allow_braking ? -q * SP_KIDIV : 0;
		if(MCI_RampCompleted(pMCI[M1])){
			MCI_ExecSpeedRamp(pMCI[M1], VescToSTM_erpm_to_speed(max_erpm), 0);
		}

	}else{
		SpeednTorqCtrlM1.PISpeed->wUpperIntegralLimit = mc_conf.s_pid_allow_braking ? -q * SP_KIDIV : 0;
		SpeednTorqCtrlM1.PISpeed->wLowerIntegralLimit = q * SP_KIDIV;
		SpeednTorqCtrlM1.PISpeed->hUpperOutputLimit = mc_conf.s_pid_allow_braking ? -q * SP_KIDIV : 0;
		SpeednTorqCtrlM1.PISpeed->hLowerOutputLimit = q;
		if(MCI_RampCompleted(pMCI[M1])){
			MCI_ExecSpeedRamp(pMCI[M1], VescToSTM_erpm_to_speed(min_erpm) , 0);
		}
	}
}

void VescToSTM_set_torque(int32_t current){
	VescToSTM_mode = STM_STATE_TORQUE;
	VescToSTM_set_open_loop(false, 0, 0);

	int q = current_to_torque(current);
	utils_truncate_number_int(&q, SpeednTorqCtrlM1.MinNegativeTorque, SpeednTorqCtrlM1.MaxPositiveTorque);

	VescToSTM_update_torque(q, mc_conf.lo_min_erpm, mc_conf.lo_max_erpm);
}

void VescToSTM_set_brake_current_rel(float val) {
	VescToSTM_set_brake(val * fabsf(mc_conf.lo_current_min)*1000);
}

void VescToSTM_set_brake_rel_int(int32_t val){
	VescToSTM_mode = STM_STATE_BRAKE;
	VescToSTM_set_open_loop(false, 0, 0);

	if(val<0) return;

	int32_t p_q = (int32_t)SpeednTorqCtrlM1.MaxPositiveTorque * val / 32768;
	int32_t n_q = (int32_t)SpeednTorqCtrlM1.MinNegativeTorque * val / 32768;

	if(p_q > SpeednTorqCtrlM1.MaxPositiveTorque){
		p_q = SpeednTorqCtrlM1.MaxPositiveTorque;
	}
	if(n_q < SpeednTorqCtrlM1.MinNegativeTorque){
		n_q = SpeednTorqCtrlM1.MinNegativeTorque;
	}
	SpeednTorqCtrlM1.PISpeed->hUpperOutputLimit = p_q;
	SpeednTorqCtrlM1.PISpeed->hLowerOutputLimit = n_q;
	SpeednTorqCtrlM1.PISpeed->wUpperIntegralLimit = p_q * SP_KIDIV;
	SpeednTorqCtrlM1.PISpeed->wLowerIntegralLimit = n_q * SP_KIDIV;
	MCI_ExecSpeedRamp(pMCI[M1], 0 , 0);
}

void VescToSTM_set_brake(int32_t current){
	VescToSTM_mode = STM_STATE_BRAKE;
	VescToSTM_set_open_loop(false, 0, 0);

	int q = current_to_torque(current);
	utils_truncate_number_int(&q, SpeednTorqCtrlM1.MinNegativeTorque, SpeednTorqCtrlM1.MaxPositiveTorque);

	if(q > 0){
		//FW_M1.wNominalSqCurr = q*q;
		//FW_M1.hDemagCurrent	= -((float)SpeednTorqCtrlM1.MaxPositiveTorque*mc_conf.foc_d_gain_scale_max_mod);
		SpeednTorqCtrlM1.PISpeed->hUpperOutputLimit = q;
		SpeednTorqCtrlM1.PISpeed->hLowerOutputLimit = -q;
		SpeednTorqCtrlM1.PISpeed->wUpperIntegralLimit = q * SP_KIDIV;
		SpeednTorqCtrlM1.PISpeed->wLowerIntegralLimit = -q * SP_KIDIV;
		MCI_ExecSpeedRamp(pMCI[M1], 0 , 0);
	}else{
		//FW_M1.wNominalSqCurr = q*q;
		//FW_M1.hDemagCurrent = ((float)SpeednTorqCtrlM1.MinNegativeTorque*mc_conf.foc_d_gain_scale_max_mod);
		SpeednTorqCtrlM1.PISpeed->hUpperOutputLimit = -q;
		SpeednTorqCtrlM1.PISpeed->hLowerOutputLimit = q;
		SpeednTorqCtrlM1.PISpeed->wUpperIntegralLimit = -q * SP_KIDIV;
		SpeednTorqCtrlM1.PISpeed->wLowerIntegralLimit = q * SP_KIDIV;
		MCI_ExecSpeedRamp(pMCI[M1], 0 , 0);
	}
}


void VescToSTM_set_speed(int32_t erpm){
	erpm *= DIR_MUL;
	VescToSTM_mode = STM_STATE_SPEED;
	if(erpm>0){
		if(erpm < mc_conf.s_pid_min_erpm) erpm = mc_conf.s_pid_min_erpm;
		if(erpm > mc_conf.lo_max_erpm) erpm = mc_conf.lo_max_erpm;
	}else{
		if(erpm > -mc_conf.s_pid_min_erpm) erpm = -mc_conf.s_pid_min_erpm;
		if(erpm < mc_conf.lo_min_erpm) erpm = mc_conf.lo_min_erpm;
	}
	VescToSTM_set_open_loop(false, 0, 0);
	if(erpm>=0){
		SpeednTorqCtrlM1.PISpeed->wUpperIntegralLimit = (int32_t)SpeednTorqCtrlM1.MaxPositiveTorque * SP_KIDIV;
		SpeednTorqCtrlM1.PISpeed->wLowerIntegralLimit = mc_conf.s_pid_allow_braking ? (int32_t)SpeednTorqCtrlM1.MinNegativeTorque * SP_KIDIV: 0;
		SpeednTorqCtrlM1.PISpeed->hUpperOutputLimit = SpeednTorqCtrlM1.MaxPositiveTorque;
		SpeednTorqCtrlM1.PISpeed->hLowerOutputLimit = mc_conf.s_pid_allow_braking ? SpeednTorqCtrlM1.MinNegativeTorque : 0;
	}else{
		SpeednTorqCtrlM1.PISpeed->wUpperIntegralLimit = mc_conf.s_pid_allow_braking ? (int32_t)SpeednTorqCtrlM1.MaxPositiveTorque * SP_KIDIV: 0;
		SpeednTorqCtrlM1.PISpeed->wLowerIntegralLimit = (int32_t)SpeednTorqCtrlM1.MinNegativeTorque * SP_KIDIV;
		SpeednTorqCtrlM1.PISpeed->hUpperOutputLimit = mc_conf.s_pid_allow_braking ? SpeednTorqCtrlM1.MaxPositiveTorque : 0;
		SpeednTorqCtrlM1.PISpeed->hLowerOutputLimit = SpeednTorqCtrlM1.MinNegativeTorque;
	}
	int32_t ramp_time = 0;
	if(mc_conf.s_pid_ramp_erpms_s) ramp_time = (float)(erpm * 1000) / mc_conf.s_pid_ramp_erpms_s;
	MCI_ExecSpeedRamp(pMCI[M1], VescToSTM_erpm_to_speed(erpm), ramp_time);
}




float VescToSTM_get_temperature(){
	return NTC_GetAvTemp_C(pMCT[M1]->pTemperatureSensor);
}
float VescToSTM_get_temperature2(){
	return 0.0;
}


float VescToSTM_get_phase_current(){
	int32_t wAux1 = SQ(FW_M1.AvAmpere_qd.q);
	int32_t wAux2 = SQ(FW_M1.AvAmpere_qd.d);
	wAux1 += wAux2;
	wAux1 = MCM_Sqrt(wAux1);
	if(abs(FW_M1.AvAmpere_qd.d) > (1.0 * CURRENT_FACTOR_A)){
		wAux1 = wAux1 * SIGN(FOCVars[M1].Iqdref.q);
	}else{
		wAux1 = wAux1 * SIGN(FW_M1.AvAmpere_qd.q);
	}

	return (float)wAux1 / CURRENT_FACTOR_A;
}

float VescToSTM_get_input_current(){
#ifdef M365
	return (float)pMPM[M1]->_super.hAvrgElMotorPowerW/(float)VBS_GetAvBusVoltage_V(pMCT[M1]->pBusVoltageSensor);
#endif

#ifdef G30P
	return (float)pMPM[M1]->_super.hAvrgElMotorPowerW/(float)VBS_GetAvBusVoltage_V(pMCT[M1]->pBusVoltageSensor);
#endif
}

/**
 * Get the amount of amp hours drawn from the input source.
 *
 * @param reset
 * If true, the counter will be reset after this call.
 *
 * @return
 * The amount of amp hours drawn.
 */
float VescToSTM_get_amp_hours(bool reset) {
	float val = (float)PQD_MotorPowMeasM1._super.dAsDraw / 360000.0;

	if (reset) {
		PQD_MotorPowMeasM1._super.dAsDraw = 0;
	}

	return val;
}


/**
 * Get the amount of watt hours drawn from the input source.
 *
 * @param reset
 * If true, the counter will be reset after this call.
 *
 * @return
 * The amount of watt hours drawn.
 */
float VescToSTM_get_watt_hours(bool reset) {
	float val = (float)PQD_MotorPowMeasM1._super.WsDraw / 3600.0;

	if (reset) {
		PQD_MotorPowMeasM1._super.WsDraw = 0;
	}

	return val;
}

/**
 * Read and reset the average direct axis motor current. (FOC only)
 *
 * @return
 * The average D axis current.
 */
float VescToSTM_get_id(){
	return (float)FW_M1.AvAmpere_qd.d / CURRENT_FACTOR_A;
}

/**
 * Read and reset the average quadrature axis motor current. (FOC only)
 *
 * @return
 * The average Q axis current.
 */
float VescToSTM_get_iq(){
	return (float)FW_M1.AvAmpere_qd.q / CURRENT_FACTOR_A;
}

/**
 * Read and reset the average direct axis motor voltage. (FOC only)
 *
 * @return
 * The average D axis voltage.
 */
float VescToSTM_get_Vd(){
	float Vin = VescToSTM_get_bus_voltage();
	float fVd = Vin / 32768.0 * (float)FW_M1.AvVolt_qd.d;
	return fVd;
}

/**
 * Read and reset the average quadrature axis motor voltage. (FOC only)
 *
 * @return
 * The average Q axis voltage.
 */
float VescToSTM_get_Vq(){
	float Vin = VescToSTM_get_bus_voltage();
	float fVq = Vin / 32768.0 * (float)FW_M1.AvVolt_qd.q;
	return fVq;
}

/**
 * Calculates the bus voltage
 *
 * @return
 * Voltage
 */
float VescToSTM_get_bus_voltage(){
	return (float)(VBS_GetAvBusVoltage_d(pMCT[M1]->pBusVoltageSensor)) / BATTERY_VOLTAGE_GAIN;
}

/**
 * Calculates the ERPM from internal average speed
 *
 * @return
 * erpm
 */
int32_t VescToSTM_get_erpm(){
	int32_t erpm = VescToSTM_speed_to_erpm(MCI_GetAvrgMecSpeedUnit( pMCI[M1] ));
	return erpm;
}

/**
 * Calculates the ERPM from internal instant speed
 *
 * @return
 * erpm
 */
int32_t VescToSTM_get_erpm_fast(){
	int32_t speed = ( (  HALL_M1._Super.hElSpeedDpp * ( int32_t )HALL_M1._Super.hMeasurementFrequency * (int32_t) SPEED_UNIT ) / (( int32_t ) HALL_M1._Super.DPPConvFactor));
	return VescToSTM_speed_to_erpm(speed);
}

/**
 * Calculates the RPM from internal average speed
 *
 * @return
 * rpm
 */
int32_t VescToSTM_get_rpm(){
	int32_t rpm = VescToSTM_speed_to_rpm(MCI_GetAvrgMecSpeedUnit( pMCI[M1] ));
	return rpm;
}

void VescToSTM_stop_motor(){
	MCI_StopMotor( pMCI[M1] );
}
void VescToSTM_start_motor(){
	MCI_StartMotor( pMCI[M1] );
}


void VescToSTM_init_odometer(mc_configuration* conf){
	tacho_scale = (conf->si_wheel_diameter * M_PI) / (3.0 * conf->si_motor_poles * conf->si_gear_ratio);
}

/**
 * Get the distance traveled based on wheel diameter, gearing and motor pole settings.
 *
 * @return
 * Distance traveled since boot, in meters
 */
float VescToSTM_get_distance(void) {
	return VescToSTM_get_tachometer_value(false) * tacho_scale;
}

/**
 * Get the absolute distance traveled based on wheel diameter, gearing and motor pole settings.
 *
 * @return
 * Absolute distance traveled since boot, in meters
 */
float VescToSTM_get_distance_abs(void) {
	return VescToSTM_get_tachometer_abs_value(false) * tacho_scale;
}

static uint32_t m_odometer_meters;
/**
 * Set odometer value in meters.
 *
 * @param new_odometer_meters
 * new odometer value in meters
 */
void VescToSTM_set_odometer(uint32_t new_odometer_meters) {
	m_odometer_meters = new_odometer_meters - VescToSTM_get_distance_abs();
}

/**
 * Return current odometer value in meters.
 *
 * @return
 * Odometer value in meters, including current trip
 */
uint32_t VescToSTM_get_odometer(void) {
	return m_odometer_meters + VescToSTM_get_distance_abs();
}

/**
 * Get the speed based on wheel diameter, gearing and motor pole settings.
 *
 * @return
 * Speed, in m/s
 */
float VescToSTM_get_speed(void) {
	float temp = (((float)MCI_GetAvrgMecSpeedUnit(pMCI[M1]) / 10.0) * mc_conf.si_wheel_diameter * M_PI) / mc_conf.si_gear_ratio / mc_conf.si_motor_poles;
	return temp * DIR_MUL;
}

/**
 * Read the number of steps the motor has rotated. This number is signed and
 * will return a negative number when the motor is rotating backwards.
 *
 * @param reset
 * If true, the tachometer counter will be reset after this call.
 *
 * @return
 * The tachometer value in motor steps. The number of motor revolutions will
 * be this number divided by (3 * MOTOR_POLE_NUMBER).
 */
int32_t VescToSTM_get_tachometer_value(bool reset) {
	int val = HALL_M1.tachometer;

	if (reset) {
		HALL_M1.tachometer = 0;
	}

	return val;
}

/**
 * Read the absolute number of steps the motor has rotated.
 *
 * @param reset
 * If true, the tachometer counter will be reset after this call.
 *
 * @return
 * The tachometer value in motor steps. The number of motor revolutions will
 * be this number divided by (3 * MOTOR_POLE_NUMBER).
 */
int32_t VescToSTM_get_tachometer_abs_value(bool reset) {
	int val = HALL_M1.tachometer_abs;

	if (reset) {
		HALL_M1.tachometer_abs = 0;
	}

	return val;
}

/**
 * Apply a fixed static current vector in open loop to emulate an electric
 * handbrake.
 *
 * @param current
 * The brake current to use.
 */
void VescToSTM_set_handbrake(float current) {

	int q = abs(current_to_torque(current*1000));
	if(VescToSTM_mode != STM_STATE_HANDBRAKE){
		VescToSTM_set_open_loop(true, SpeednTorqCtrlM1.SPD->hElAngle, 0);
	}
	VescToSTM_mode = STM_STATE_HANDBRAKE;

	utils_truncate_number_int(&q, 0, SpeednTorqCtrlM1.MaxPositiveTorque);

	pMCI[M1]->pSTC->PISpeed->hUpperOutputLimit = q;
	pMCI[M1]->pSTC->PISpeed->hLowerOutputLimit = q;
	MCI_ExecSpeedRamp(pMCI[M1], 0 , 0);
}

/**
 * Get the battery level, based on battery settings in configuration. Notice that
 * this function is based on remaining watt hours, and not amp hours.
 *
 * @param wh_left
 * Pointer to where to store the remaining watt hours, can be null.
 *
 * @return
 * Battery level, range 0 to 1
 */
float VescToSTM_get_battery_level(float *wh_left) {
	const volatile mc_configuration *conf = &mc_conf;
	const float v_in = VescToSTM_get_bus_voltage();
	float battery_avg_voltage = 0.0;
	float battery_avg_voltage_left = 0.0;
	float ah_left = 0;
	float ah_tot = conf->si_battery_ah;

	switch (conf->si_battery_type) {
#if BATTERY_SUPPORT_LIION
	case BATTERY_TYPE_LIION_3_0__4_2:
		battery_avg_voltage = ((3.2 + 4.2) / 2.0) * (float)(conf->si_battery_cells);
		battery_avg_voltage_left = ((3.2 * (float)(conf->si_battery_cells) + v_in) / 2.0);
		float batt_left = utils_map(v_in / (float)(conf->si_battery_cells),
									3.2, 4.2, 0.0, 1.0);
		batt_left = utils_batt_liion_norm_v_to_capacity(batt_left);
		ah_tot *= 0.85; // 0.85 because the battery is not fully depleted at 3.2V / cell
		ah_left = batt_left * ah_tot;
		break;
#endif
#if BATTERY_SUPPORT_LIFEPO
	case BATTERY_TYPE_LIIRON_2_6__3_6:
		battery_avg_voltage = ((2.8 + 3.6) / 2.0) * (float)(conf->si_battery_cells);
		battery_avg_voltage_left = ((2.8 * (float)(conf->si_battery_cells) + v_in) / 2.0);
		ah_left = utils_map(v_in / (float)(conf->si_battery_cells),
				2.6, 3.6, 0.0, conf->si_battery_ah);
		break;
#endif
#if BATTERY_SUPPORT_LEAD
	case BATTERY_TYPE_LEAD_ACID:
		// TODO: This does not really work for lead-acid batteries
		battery_avg_voltage = ((2.1 + 2.36) / 2.0) * (float)(conf->si_battery_cells);
		battery_avg_voltage_left = ((2.1 * (float)(conf->si_battery_cells) + v_in) / 2.0);
		ah_left = utils_map(v_in / (float)(conf->si_battery_cells),
				2.1, 2.36, 0.0, conf->si_battery_ah);
		break;
#endif

	default:
		break;
	}

	const float wh_batt_tot = ah_tot * battery_avg_voltage;
	const float wh_batt_left = ah_left * battery_avg_voltage_left;

	if (wh_left) {
		*wh_left = wh_batt_left;
	}

	return wh_batt_left / wh_batt_tot;
}

float VescToSTM_get_duty_cycle_now(void) {
	if(PWM_Handle_M1._Super.PWM_off){
		return 1.0 / VescToSTM_get_bus_voltage() * motor_voltage;
	}else{
		int16_t u16Ampl = FW_M1.AvVoltAmpl * SIGN(FW_M1.AvVolt_qd.q);
		return u16Ampl / 32768.0;
	}
}


float VescToSTM_get_duty_cycle_now_fast(void) {
	int32_t AvVoltAmpl = MCM_Sqrt(SQ(FW_M1.AvVolt_qd.q) + SQ(FW_M1.AvVolt_qd.d))* SIGN(FW_M1.AvVolt_qd.q);
	return AvVoltAmpl / 32768.0;
}

/**
 * Set current relative to the minimum and maximum current limits.
 *
 * @param current
 * The relative current value, range [-1.0 1.0]
 */
void VescToSTM_set_current_rel(float val) {
	VescToSTM_mode = STM_STATE_TORQUE;
	VescToSTM_set_open_loop(false, 0, 0);
	int q;
	float torque;
	if(val>0){
		torque = (float)SpeednTorqCtrlM1.MaxPositiveTorque * val;
		q = torque;
	}else{
		torque = (float)SpeednTorqCtrlM1.MinNegativeTorque * val;
		q = torque;
		q = q *-1;
	}

	utils_truncate_number_int(&q, SpeednTorqCtrlM1.MinNegativeTorque, SpeednTorqCtrlM1.MaxPositiveTorque);

	VescToSTM_update_torque(q, mc_conf.lo_min_erpm, mc_conf.lo_max_erpm);
}

/**
 * Set current relative to the minimum and maximum current limits.
 *
 * @param current
 * The relative current value, range [-32768 +32768]
 */
void VescToSTM_set_current_rel_int(int32_t val) {
	VescToSTM_mode = STM_STATE_TORQUE;
	VescToSTM_set_open_loop(false, 0, 0);
	int q;
	if(val>0){
		q = (int32_t)SpeednTorqCtrlM1.MaxPositiveTorque * val / 32768;
	}else{
		q = (int32_t)SpeednTorqCtrlM1.MinNegativeTorque * val / 32768;
		q = q *-1;
	}

	utils_truncate_number_int(&q, SpeednTorqCtrlM1.MinNegativeTorque, SpeednTorqCtrlM1.MaxPositiveTorque);

	VescToSTM_update_torque(q, mc_conf.lo_min_erpm, mc_conf.lo_max_erpm);
}

mc_fault_code VescToSTM_get_fault(void) {
	switch(pMCI[M1]->pSTM->hFaultOccurred){
	case MC_NO_ERROR:
		return FAULT_CODE_NONE;
		break;
	case MC_OVER_TEMP:
		return FAULT_CODE_OVER_TEMP_FET;
		break;
	case MC_OVER_VOLT:
		return FAULT_CODE_OVER_VOLTAGE;
		break;
	case MC_UNDER_VOLT:
		return FAULT_CODE_UNDER_VOLTAGE;
		break;
	case MC_BREAK_IN:
		return FAULT_CODE_ABS_OVER_CURRENT;
		break;
	default:
		return FAULT_CODE_NONE;
	}

}
