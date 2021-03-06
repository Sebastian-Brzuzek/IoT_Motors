#define ICACHE_FLASH

#include "c_types.h"
#include "command_layer.h"
#include "hw_timer.h"
#include "jsmn.h"
#include "udp.h"
#include "mem.h"
#include <math.h>
#include "motor_driver.h"
#include "op_queue.h"
#include "osapi.h"
#include "tcp.h"
#include "user_config.h"
#include "user_interface.h"
#include "wifi.h"

#define JSON_TOKEN_AMOUNT 83

os_event_t task_queue[TASK_QUEUE_LENGTH];

struct stepper_command_packet command[4];
uint8 command_address[4][4];

//static volatile jsmntok_t tokens[JSON_TOKEN_AMOUNT];
static volatile jsmn_parser json_parser; 

void initialize_command_layer()
{
	init_motor_gpio();
	register_motor_packet_callback(*motor_process_command);
	register_wifi_packet_callback(*wifi_process_command);
	register_tcp_json_callback(*json_process_command);
	
	system_os_task(acknowledge_command, ACK_TASK_PRIO, task_queue, TASK_QUEUE_LENGTH);
	system_os_task(driver_logic_task, MOTOR_DRIVER_TASK_PRIO, task_queue, TASK_QUEUE_LENGTH);
	
	hw_timer_init(FRC1_SOURCE, 1);
	hw_timer_set_func(step_driver);
	hw_timer_arm(RESOLUTION_US);
	system_init_done_cb(wifi_init);
}

void motor_process_command(struct stepper_command_packet *packet, uint8 *ip_addr)
{
	if (packet->queue && ( !is_queue_empty(packet->motor_id) ||  is_motor_running(packet->motor_id) ) )
	{
		store_command(packet, ip_addr, packet->motor_id);
	}
	else
	{
		command[packet->motor_id] = *packet;
		command_address[packet->motor_id][0] = ip_addr[0];
		command_address[packet->motor_id][1] = ip_addr[1];
		command_address[packet->motor_id][2] = ip_addr[2];
		command_address[packet->motor_id][3] = ip_addr[3];
		issue_command(packet->motor_id);
		clear_queue(packet->motor_id);
	}
	
}

void wifi_process_command(struct wifi_command_packet *packet, uint8 *ip_addr)
{
	if(packet->opcode == 'C')
	{
		char *ssid = packet->ssid;
		char *pass =  packet->password;
		change_opmode(STATION_CONNECT, ssid, pass);
	}
	else if(packet->opcode == 'D')
	{
		os_printf("Disconnect from this network and resume broadcasting\n");
		change_opmode(BROADCAST, "", "");
	}
	else if(packet->opcode == 'N')
	{
		os_printf("Mesh Node\n");
	}
	else
	{
		os_printf("Opcode not found!\n");
	}
	
}

void ICACHE_FLASH_ATTR json_process_command(char *json_input)
{
	jsmn_init(&json_parser);
	jsmntok_t tokens[JSON_TOKEN_AMOUNT] = {0};
	int len = jsmn_parse(&json_parser, json_input, os_strlen(json_input), tokens, JSON_TOKEN_AMOUNT);
	if(len < 0)
	{
		os_printf("JSON Parsing error code %d\n", len);
		return;
	}
	int place = 0;
	while(place < len)
	{
		os_printf("JSON token %d\n");
		switch(tokens[place].type)
		{
			case JSMN_STRING:
				if(os_memcmp("code", json_input + tokens[place].start, os_strlen("code")) == 0)
				{
					os_printf("opcode\n");
					char json_opcode = *(json_input + tokens[place+1].start);
					uint8 dummy_ip[4];
					switch(json_opcode)
					{
						case 'C':
						case 'D':
						{
							struct wifi_command_packet parsed_wifi_command;
							parsed_wifi_command.opcode = json_opcode;
							int token_len = tokens[place+4].end - tokens[place+4].start;
							os_strncpy(parsed_wifi_command.ssid, json_input + tokens[place+4].start, token_len);
							parsed_wifi_command.ssid[token_len] = 0;
							token_len = tokens[place+5].end - tokens[place+5].start;
							os_strncpy(parsed_wifi_command.password, json_input + tokens[place+5].start, token_len);
							parsed_wifi_command.password[token_len] = 0;
							wifi_process_command(&parsed_wifi_command, dummy_ip);
							place += 6;
							break;
						}
						case 'M':
						case 'S':
						case 'G':
						{
							os_printf("M, S or G\n");
							struct stepper_command_packet parsed_motor_command;
							parsed_motor_command.port = 0;
							parsed_motor_command.opcode = json_opcode;
							parsed_motor_command.motor_id = 0;
							parsed_motor_command.queue = (*(json_input + tokens[place+4].start) == '1') ? 0x01 : 0x00;
							signed int steps = 0;
							int place_tracker = 0;
							int negative = 0;
							if(*(json_input + tokens[place+5].start) == '-')
							{
								negative = 1;
								place_tracker++;
							}
							for(place_tracker; place_tracker < (tokens[place+5].end - tokens[place+5].start); place_tracker++)
							{
								steps *= 10;
								steps += *(json_input + tokens[place+5].start + place_tracker) - 48;
							}
							steps = (negative == 1) ? ((-1) * steps) : steps;
							parsed_motor_command.step_num = ntohl(steps);
							unsigned short rate = 0;
							place_tracker = 0;
							for(place_tracker; place_tracker < (tokens[place+6].end - tokens[place+6].start); place_tracker++)
							{
								rate *= 10;
								rate += *(json_input + tokens[place+6].start + place_tracker) - 48;
							}
							parsed_motor_command.step_rate = ntohs(rate);
							motor_process_command(&parsed_motor_command, dummy_ip);
							place +=7;
							break;
						}
						case 'R':
							//steps per degree
						{
							break;
						}
						case 'U':
						{
							os_printf("U\n");
							struct stepper_command_packet parsed_motor_command;
							parsed_motor_command.port = 0;
							parsed_motor_command.opcode = json_opcode;
							parsed_motor_command.motor_id = 0;
							parsed_motor_command.queue = (*(json_input + tokens[place+4].start) == '1') ? 0x01 : 0x00;
							parsed_motor_command.step_num = 0;
							unsigned short ustep_setting = *(json_input + tokens[place+5].start) - 48;
							parsed_motor_command.step_rate = ntohs(ustep_setting);
							motor_process_command(&parsed_motor_command, dummy_ip);
							place += 6;
							break;
						}
						case 'B':
							//servo bounds
						{
							os_printf("B\n");
							struct stepper_command_packet stop_packet;
							stop_packet.queue = 0;
							stop_packet.opcode = 'S';
							stop_packet.port = 0;
							stop_packet.step_num = 0;
							stop_packet.step_rate = 0;
							stop_packet.motor_id = 0;
							command[0] = stop_packet;
							command_address[0][0] = 0;
							command_address[0][1] = 0;
							command_address[0][2] = 0;
							command_address[0][3] = 0;
							issue_command(0);
							clear_queue(0);
							int place_tracker = 0;
							int bound = 0;
							for(place_tracker; place_tracker < (tokens[place+4].end - tokens[place+4].start); place_tracker++)
							{
								bound *= 10;
								bound += *(json_input + tokens[place+4].start + place_tracker) - 48;
							}
							change_motor_setting(MIN_SERVO_BOUND, bound);
							os_printf("Min: %d\n", bound);
							bound = 0;
							place_tracker = 0;
							for(place_tracker; place_tracker < (tokens[place+5].end - tokens[place+5].start); place_tracker++)
							{
								bound *= 10;
								bound += *(json_input + tokens[place+5].start + place_tracker) - 48;
							}
							change_motor_setting(MAX_SERVO_BOUND, bound);
							os_printf("Max: %d\n", bound);
							place += 6;
							break;
						}
						default:
							os_printf("Opcode %c not found!\n", json_opcode);
							return;
							break;
					}
					break;
				}
			case JSMN_OBJECT:
			case JSMN_ARRAY:
				os_printf("JSON Object or Array %d\n");
				place++;
				break;
			case JSMN_PRIMITIVE:
			case JSMN_UNDEFINED:
				os_printf("Malformed JSON query!\n");
				return;
				break;
		}
	}
}

void issue_command(char motor_id)
{
	
	if(command[motor_id].opcode == 'S')
    {
        //os_printf("Stop Command, %d Delay Counts at %u counts per second\n",
            //ntohl(command.step_num), ntohs(command.step_rate));
	    opcode_stop(ntohl(command[motor_id].step_num), ntohs(command[motor_id].step_rate), motor_id);
    }
    else if(command[motor_id].opcode == 'M')
    {
        //os_printf("Move Command, %d Relative Steps at %u steps per second\n",
            //ntohl(command.step_num), ntohs(command.step_rate));
	    opcode_move(ntohl(command[motor_id].step_num), ntohs(command[motor_id].step_rate), motor_id);
    }
    else if(command[motor_id].opcode == 'G')
    {
        //os_printf("Goto Command, %d Absolute Steps at %u steps per second\n",
            //ntohl(command.step_num), ntohs(command.step_rate));
	    opcode_goto(ntohl(command[motor_id].step_num), ntohs(command[motor_id].step_rate), motor_id);
    }
    else if(command[motor_id].opcode == 'U')
    {
    	change_motor_setting(MICROSTEPPING, ntohs(command[motor_id].step_rate));
    }
    else
    {
        os_printf("Opcode not found!\n");
        return;
    }
	
}

void fetch_command(char motor_id)
{
	if(!is_queue_empty(motor_id))
	{
		command[motor_id] = get_command(motor_id)->packet;
		os_memcpy(command_address[motor_id], get_command(motor_id)->ip_addr, 4);
		remove_first_command(motor_id);
		issue_command(motor_id);
	}
}

void acknowledge_command(os_event_t *events)
{
	if(command[events->sig].port != 0) udp_send_ack(command[events->sig].opcode, 0, command_address[events->sig], ntohs(command[events->sig].port));
	fetch_command(events->sig);
}