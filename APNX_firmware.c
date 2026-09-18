/*
             LUFA Library
     Copyright (C) Dean Camera, 2021.

  dean [at] fourwalledcubicle [dot] com
           www.lufa-lib.org
*/

/*
  Copyright 2021  Dean Camera (dean [at] fourwalledcubicle [dot] com)

  Permission to use, copy, modify, distribute, and sell this
  software and its documentation for any purpose is hereby granted
  without fee, provided that the above copyright notice appear in
  all copies and that both that the copyright notice and this
  permission notice and warranty disclaimer appear in supporting
  documentation, and that the name of the author not be used in
  advertising or publicity pertaining to distribution of the
  software without specific, written prior permission.

  The author disclaims all warranties with regard to this
  software, including all implied warranties of merchantability
  and fitness.  In no event shall the author be liable for any
  special, indirect or consequential damages or any damages
  whatsoever resulting from loss of use, data or profits, whether
  in an action of contract, negligence or other tortious action,
  arising out of or in connection with the use or performance of
  this software.
*/

/* 
 * Based on LUFA by Dean Camera
 * Modified and extended by JaemoPark
*/

/** \file
 *
 *  Main source file for the Bulk Vendor demo. This file contains the main tasks of the demo and
 *  is responsible for the initial application hardware configuration.
 */

#define  INCLUDE_FROM_BULKVENDOR_C
#include "BulkVendor.h"

// 워치독 설정 & 인터럽트 설정
#include <avr/wdt.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <avr/io.h>

#define BOOT_KEY       0x7777		// 부트로더에서 의도적인 리셋임을 밝히는 키(부트로더 모드로 계속 유지함)
#define BOOT_KEY_ADDR  ((uint16_t*)0x0800)		// 부트로더 키(카타리나 부트로더 기준)
#define GO_TO_BOOT		0x77					// cmd가 0xff일시 부트로더로 감

#define MAX_PACKET_LEN 255

typedef enum {
    CHECK_OK = 0,
    CHECK_ERR_CMD,	   			// 유효한 명령이 아님
	CHECK_ERR_COUNT,   			// 블록 수 초과/혹은 유효하지 않음
    CHECK_ERR_CMD_TYPE,			// 유효한 타입이 아님(SS, SB가 아닌 값)
	CHECK_ERR_DATA_TYPE,	   // 유효한 데이터 타입이 아님
    CHECK_ERR_ADDRESS, 		   // 유효한 주소가 아님
	CHECK_GO_TO_BOOT,		   // 부트로더로 이동
} check_result_t;

// 에러 코드 (프로토콜 - 호스트에 전송)
typedef enum {
    ERR_NONE            = 0x0000,  // 에러 없음 (사용 안 함)
    
    // 0x00xx: 명령 관련 에러
    ERR_INVALID_CMD     = 0x0001,  // 유효하지 않은 명령
    ERR_INVALID_CMD_TYPE= 0x0002,  // 유효하지 않은 명령 타입 (SS/SB)
    
    // 0x01xx: 데이터 관련 에러
    ERR_INVALID_DATA_TYPE = 0x0101, // 유효하지 않은 데이터 타입
    ERR_INVALID_COUNT     = 0x0102, // 카운트 범위 초과 (>16)
    
    // 0x02xx: 주소 관련 에러  
    ERR_INVALID_ADDRESS   = 0x0201, // 주소 범위 초과
    ERR_OUT_OF_RANGE      = 0x0202, // 데이터 범위 초과
    
    // 0xFFxx: 시스템 에러
    ERR_INTERNAL          = 0xFF01, // 내부 에러
    ERR_UNKNOWN           = 0xFFFF  // 알 수 없는 에러
} error_code_t;

// 커맨드
typedef enum {
	// 수신 전용 커맨드
	READ = 0,	
	WRITE,

	// 송신 커맨드
	ACK = 5,
	NAK = 15
} command_t;

typedef enum {
	DATA_TYPE_8 = 0,	// 8bit
	DATA_TYPE_16,	// 16bit
	DATA_TYPE_32,	// 32bit
	DATA_TYPE_64		// 64bit
} command_type_t;

typedef enum {
    ADDR_INPUT = 0x100,
    ADDR_OUTPUT = 0x200,
    ADDR_DATA = 0x300,
    ADDR_FLAG = 0x400
} logical_addr_t;



// 프로토콜에서 주소 부분이 사용할 주소 
struct Address_list {
	uint8_t input[16];		// (0x100부터 시작 (논리 주소 형태))
	uint8_t output[16];		// (0x200부터 시작(논리 주소))
	uint8_t data[64];		// 산술 연산?을 담당하는 변수 (주로 여기서 테스트할 예정) (0x300부터 시작 (논리 주소))
	uint8_t flag[32];		// (0x400부터 시작 (논리 주소))
} memory_map;


/*
 * 응답 헤더 입니다 패킷입니다, 여기선 기존 stx, len, crc 와 드라이버가 검증한 부분은 
 * 제외하고 검증합니다
*/
struct recived_packet_hdr {
	uint8_t len;
	uint8_t id;					// Device id(미정)
	uint8_t cmd;				// 명령어(read, write, 등)
	uint8_t cmd_type; 			// 단일 주소 체계인지, 연속 주소 체계인지 (구현 예정)
	uint8_t data_type;			// 데이터 타입(uint8, uint16, uint32, uint64등인지..)
	uint8_t count;				// 읽고 쓸 데이터 갯수
};

struct recived_packet {
	// 헤더
	struct recived_packet_hdr header;
	// 가변 길이를 raw data로 취급
	uint8_t *payload;
};

// __attribute__((packed))를 통해서 패딩을 줄임
struct send_packet_hdr {
	uint8_t stx;
	uint8_t len;
	uint8_t id;
	uint8_t cmd;
	uint8_t cmd_type;
}__attribute__((packed));

struct write_ack_packet {
	struct send_packet_hdr header;
	uint16_t crc;
}__attribute__((packed));

struct read_ack_packet {
	struct send_packet_hdr header;
	uint8_t data_type;
	uint8_t payload[1];
	uint16_t crc;
}__attribute__((packed));

struct nak_packet {
	struct send_packet_hdr header;
	uint16_t error_code;
	uint16_t crc;
}__attribute__((packed));

// 읽을때의 상태머신
struct rx_fsm {
    check_result_t state;		// 상태
    uint8_t expected_len;			// len을 읽어와서 현재 상태
	uint8_t buf[MAX_PACKET_LEN];	// 최대 버퍼
	int copied_len;
};


// 유틸리티 함수

uint16_t be_to_le16(uint8_t *data) {
	
	uint16_t le_data = (*(data+1) << 8) | *data;
	
	return le_data;
} 

uint16_t le_to_be16(uint8_t *data) {
	
	uint16_t be_data = (*(data+1) << 8) | *data;
	
	return be_data;
} 

uint32_t be_to_le32(uint8_t *data) {
	return (uint32_t)data[3] << 24 |
		   (uint32_t)data[2] << 16 |
		   (uint32_t)data[1] << 8  |
		   (uint32_t)data[0];
}

uint32_t le_to_be32(uint8_t *data) {
	return (uint64_t)data[3] << 24 |
		   (uint64_t)data[2] << 16 |
		   (uint64_t)data[1] << 8  |
		   (uint64_t)data[0];
}


// 64비트 빅 엔디안 값을 리틀 엔디안으로 변환시키는 함수
uint64_t be_to_le64(uint8_t *data) {

    return (uint64_t)data[7] << 56 |
		   (uint64_t)data[6] << 48 |
		   (uint64_t)data[5] << 40 |
		   (uint64_t)data[4] << 32 |
		   (uint64_t)data[3] << 24 |
		   (uint64_t)data[2] << 16 |
		   (uint64_t)data[1] << 8  |
		   (uint64_t)data[0]; 
}

uint64_t le_to_be64(uint8_t *data) {

    return (uint64_t)data[7] << 56 |
		   (uint64_t)data[6] << 48 |
		   (uint64_t)data[5] << 40 |
		   (uint64_t)data[4] << 32 |
		   (uint64_t)data[3] << 24 |
		   (uint64_t)data[2] << 16 |
		   (uint64_t)data[1] << 8  |
		   (uint64_t)data[0]; 
}

// 패킷 처리 관련 함수

// 패킷 파싱하는 함수
static void parse_packet(struct rx_fsm *rx, struct recived_packet *recv) {
	uint8_t *buf = rx->buf;

	recv->header.len = buf[1];
    recv->header.id = buf[2];
    recv->header.cmd = buf[3];
    recv->header.cmd_type = buf[4];
    recv->header.data_type = buf[5];
    recv->header.count = buf[6];

	// payload 부분은 raw데이터의 주소를 저장함(이후에 execute 단계에서 파싱)
	recv->payload = &buf[7];
}

// command가 유효한지 확인하는 함수
static int is_valid_command(uint8_t cmd) {
	
	// 만약 부트로더로 가는 명령인지 일차적으로 검증
	if(cmd == GO_TO_BOOT)
		return 2;

	if (cmd < READ || cmd > WRITE)
		return 0;
	return 1;
}

static int is_valid_cmd_type(uint8_t cmd_type) {
	if (cmd_type != 1) {		//SB는 아직 미구현 상태
		return 0;
	}
	return 1;
}

static int is_valid_count(uint8_t count) {
	if (count > 16 || count < 1) 
		return 0;
	return 1;
}

// command type가 유효한지 확인하는 함수
static int is_valid_data_type(uint8_t type) {
	if (type < DATA_TYPE_8 || type > DATA_TYPE_64)
		return 0;
	return 1;
}

// 주소가 유효한지 확인하는 함수
static int is_valid_address(uint16_t address, uint8_t data_type){
	// address를 마스킹함
	uint16_t addr_type = address & 0xF00;		// 3번째 자리만 마스킹해서 타입을 구함
	uint16_t offset = address & ~addr_type;
	switch (addr_type)
	{
	case ADDR_INPUT:		//0x100
		if (offset+data_type < sizeof(memory_map.input))
			return 1;
		break;
	case ADDR_OUTPUT:		//0x200
		if (offset+data_type < sizeof(memory_map.output))
			return 1;
		break;
	case ADDR_DATA:			//0x300
		if (offset+data_type < sizeof(memory_map.data))
			return 1;
		break;
	case ADDR_FLAG:			//0x400
		if (offset+data_type < sizeof(memory_map.flag))
			return 1;
		break;
	default:				// 이외 잘못된 주소
		return 0;
	}

	return 0;
}

static uint8_t data_size_by_type(uint8_t type) {
	
	switch (type)
	{
	case DATA_TYPE_8:
		return 1;
	case DATA_TYPE_16:
		return 2;
	case DATA_TYPE_32:
		return 4;
	case DATA_TYPE_64:
		return 8;
	default:
		return 0;
	}
}

static check_result_t check_packet(struct rx_fsm *rx) {
	// check_packet 에서는 단지 검증만 하면됨, 굳이 switch_case로 상태머신을 만들 필요가 없음
	// 이미 main함수에서 필요한만큼 데이타를 얻음

	int retval;

	uint8_t *buf = rx->buf;

    uint8_t cmd	= buf[3];
	uint8_t cmd_type = buf[4];
	uint8_t data_type = buf[5];
    uint8_t count = buf[6];
    

	// 순차적 검증
	retval = is_valid_command(cmd);
	if (retval == 2)
		return CHECK_GO_TO_BOOT;
	else if (!retval) 
        return CHECK_ERR_CMD;
	
	retval = is_valid_cmd_type(cmd_type);
	if(!retval)
		return CHECK_ERR_CMD_TYPE;
	
	retval = is_valid_count(count);
	if(!retval)
		return CHECK_ERR_COUNT;

	retval = is_valid_data_type(data_type);
	if(!retval)
		return CHECK_ERR_DATA_TYPE;

	// 이후 주소와 데이터 필드는 가변길이
	uint16_t offset = 7;
    uint16_t addr = 0;
	// data_type에 대한 사이즈
	uint8_t data_size = data_size_by_type(data_type);

	// count갯수 만큼 가변적으로 데이터가 있음
	for (int i = 0; i < count; i++) {
        addr = be_to_le16(buf + offset);
        if (!is_valid_address(addr,data_size))
            return CHECK_ERR_ADDRESS;
        offset += 2;

        if (cmd == WRITE) {
			// write일 경우에 address 다음에 data필드가 있어서 그 부분도 지나가야함
            offset += data_size;
        }
    }
    
    return CHECK_OK;
}

// 명령 실행 및 응답 생성
static size_t execute_command(struct recived_packet *recv, uint8_t *tx_buffer) {
    
	// read 명령이면
    if (recv->header.cmd == READ) {
		return execute_read(recv, tx_buffer);
	// write 명령이면
    } else if (recv->header.cmd == WRITE) {
		return execute_write(recv, tx_buffer);   
    }
    
	return 0;
}


// 받은 프로토콜을 검증하는 함수
static int check_packet(struct rx_fsm *rx);

// 헬퍼 함수들

// CRC 생성(모드버스 방식으로 생성)
uint16_t calculate_crc(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001)
                crc = (crc >> 1) ^ 0xA001;
            else
                crc >>= 1;
        }
    }
    return crc;
}

// 응답 전송
static void send_response(uint8_t *pkt) {
    Endpoint_SelectEndpoint(VENDOR_IN_EPADDR);
    Endpoint_Write_Stream_LE(pkt, sizeof(*pkt), NULL);
    Endpoint_ClearIN();
}


// WRITE ACK 생성
static size_t build_write_ack_packet(uint8_t *tx_buffer, uint8_t cmd_type) {
    
	// tx_buffer을 write_ack_packet으로 형변환 시킴
    struct write_ack_packet *ack = (struct write_ack_packet*)tx_buffer;
    
    ack->header.stx = 0x02;
    ack->header.len = sizeof(struct write_ack_packet);
    ack->header.id = 0x01;
    ack->header.cmd = ACK;
    ack->header.cmd_type = cmd_type;
    
    // CRC (payload 끝에 추가)
    uint16_t crc = calculate_crc(tx_buffer, ack->header.len - sizeof(uint16_t));
    // CRC도 엔디안에 맞게 배치(빅엔디안 형식)
    tx_buffer[ack->header.len-2] = (uint8_t)(crc >> 8);   // 상위 바이트를 앞 주소에
    tx_buffer[ack->header.len-1] = (uint8_t)(crc & 0xFF); // 하위 바이트를 뒷 주소에
    
    return sizeof(struct write_ack_packet);
}

// READ ACK 생성
static size_t build_read_ack_packet(
    uint8_t *tx_buffer, 
    uint8_t cmd_type,
    uint8_t data_type,
    const uint8_t *data,
    size_t data_size,
	uint8_t count
) {

	// tx_buffer에 쓰기 위해서 tx_Buffer을 read_ack_packet으로 형변환(메모리 접근 방식 변경)
    struct read_ack_packet *ack = (struct read_ack_packet*)tx_buffer;
    
    // 전체 크기 계산
    size_t total_size = sizeof(struct send_packet_hdr) + 1 + data_size + sizeof(uint16_t);
    
    // 헤더
    ack->header.stx = 0x02;
    ack->header.len = total_size;
    ack->header.id = 0x01;
    ack->header.cmd = ACK;
    ack->header.cmd_type = cmd_type;
    
    // 데이터 타입
    ack->data_type = data_type;
	ack->count = count;
    
    // 페이로드 복사
    memcpy(ack->payload, data, data_size);
    
    // CRC (payload 끝에 추가)
    uint16_t crc = calculate_crc(tx_buffer, total_size - sizeof(uint16_t));
    // CRC도 엔디안에 맞게 배치(빅엔디안 형식)
    tx_buffer[total_size-2] = (uint8_t)(crc >> 8);   // 상위 바이트를 앞 주소에
    tx_buffer[total_size-1] = (uint8_t)(crc & 0xFF); // 하위 바이트를 뒷 주소에

    return total_size;
}


// NAK 생성
static size_t build_nak_packet(uint8_t *tx_buffer, uint8_t cmd_type, error_code_t error_code) {
    
	// tx_buffer을 send_nak_packet*으로 형변환 하여 접근
	// 캐스팅으로 단지 해석하는 방법만 달리하여 접근
	struct nak_packet *nak = (struct nak_packet*)tx_buffer;

    nak->header.stx = 0x02;
    nak->header.len = sizeof(struct nak_packet);
    nak->header.id = 0x01;
    nak->header.cmd = NAK;
    nak->header.cmd_type = cmd_type;
    nak->error_code = error_code;

	// CRC (payload 끝에 추가)
    uint16_t crc = calculate_crc(tx_buffer, nak->header.len - sizeof(uint16_t));
    // CRC도 엔디안에 맞게 배치(빅엔디안 형식)
    tx_buffer[nak->header.len-2] = (uint8_t)(crc >> 8);   // 상위 바이트를 앞 주소에
    tx_buffer[nak->header.len-1] = (uint8_t)(crc & 0xFF); // 하위 바이트를 뒷 주소에
    
    return sizeof(struct nak_packet);
}


// 비정상 요청에 대한 응답을 보내는 함수
static void generate_error_response(uint8_t *tx_buffer, check_result_t error_type, uint8_t cmd_type) {
	
	switch (error_type)
	{
	case CHECK_ERR_CMD:
		build_nak_packet(tx_buffer, cmd_type, ERR_INVALID_CMD);
		break;
	case CHECK_ERR_COUNT:
		build_nak_packet(tx_buffer, cmd_type, ERR_INVALID_COUNT);
		break;
	case CHECK_ERR_CMD_TYPE:
		build_nak_packet(tx_buffer, cmd_type, ERR_INVALID_CMD_TYPE);
		break;
	case CHECK_ERR_DATA_TYPE:
		build_nak_packet(tx_buffer, cmd_type, ERR_INVALID_DATA_TYPE);
		break;
	case CHECK_ERR_ADDRESS:
		build_nak_packet(tx_buffer, cmd_type, ERR_INVALID_ADDRESS);
		break;
	default:
		break;
	}
}

// 내부 연산 처리용 함수

// 읽기 명령을 처리하는 함수

// dataType별로 읽는 함수입니다
static void read_data_by_type(uint8_t *addr, uint8_t *read_data, uint8_t data_type) {
    switch (data_type) {
    case DATA_TYPE_8:
        *read_data = *(uint8_t *)addr;
        break;
    case DATA_TYPE_16:
            // 여기서 직접 read_data를 swap하면서 big-endian으로 배열을 바꿔야함
            read_data[0] = addr[1];
            read_data[1] = addr[0];
            break;
    case DATA_TYPE_32:
        // 여기서 직접 read_data를 swap하면서 big-endian으로 배열을 바꿔야함
            read_data[0] = addr[3];
            read_data[1] = addr[2];
            read_data[2] = addr[1];
            read_data[3] = addr[0];
            break;
        break;
    case DATA_TYPE_64:
        // 여기서 직접 read_data를 swap하면서 big-endian으로 배열을 바꿔야함
            read_data[0] = addr[7];
            read_data[1] = addr[6];
            read_data[2] = addr[5];
            read_data[3] = addr[4];
            read_data[4] = addr[3];
            read_data[5] = addr[2];
            read_data[6] = addr[1];
            read_data[7] = addr[0];
        break;
    }
}

static void read_memory(uint16_t addr, uint8_t data_type, uint8_t *read_data) {
	// address를 마스킹함
	uint16_t addr_base = addr & 0xF00;		// 3번째 자리만 마스킹해서 타입을 구함
	uint16_t offset = addr & ~addr_base;		// 마스킹함

	// 마스킹 한 주소의 첫 필드를 매핑해둔 주소와 비교하여, 주소필드의 첫번째 주소에 더함
	switch (addr_base)
	{
	case ADDR_INPUT:		//0x100
		read_data_by_type(memory_map.input+offset, read_data, data_type);
		break;
	case ADDR_OUTPUT:		//0x200
		read_data_by_type(memory_map.output+offset, read_data, data_type);
		break;
	case ADDR_DATA:			//0x300
		read_data_by_type(memory_map.data+offset, read_data, data_type);
		break;
	case ADDR_FLAG:			//0x400
		read_data_by_type(memory_map.flag+offset, read_data, data_type);
		break;
	default:				// 이외 잘못된 주소
		return;
	}
}

// READ 명령 실행
static size_t execute_read(struct recived_packet *recv, uint8_t *tx_buffer) {
    
    uint8_t count = recv->header.count;
    uint8_t data_type = recv->header.data_type;
    uint8_t data_size = data_size_by_type(data_type);
    uint8_t *payload = recv->payload;
    
    // 읽은 데이터를 담을 버퍼
    uint8_t read_data[128];
    size_t read_offset = 0;
    
    // payload에서 주소를 파싱하고 해당 주소에서 데이터 읽기
    size_t payload_offset = 0;
    
    for (int i = 0; i < count; i++) {
        // 주소 추출
        uint16_t addr = be_to_le16(payload + payload_offset);
        payload_offset += 2;
        
        // 주소에서 데이터 읽기
        read_memory(addr, data_type, read_data + read_offset);
        read_offset += data_size;
    }
    
    // READ ACK 패킷 생성
    return build_read_ack_packet(
        tx_buffer,
        recv->header.cmd_type,
        recv->header.data_type,
        read_data,
        read_offset,
		count
    );
}

// switch-case문 안에 변수를 선언하고 초기화 하는것을 허용하지 않음(switch문 안에서 실행이 어디로 튈지 모르는데(Jump), 그 중간에 변수를 새로 만드는 것이 문법적으로 모호하기 때문)
static void write_data_by_type(uint8_t *addr, uint8_t *write_data, uint8_t data_type) {
    switch (data_type) {
    case DATA_TYPE_8:
            *(uint8_t *)addr = *write_data;
            break;
    case DATA_TYPE_16:
    {       // <-- 중괄호를 열어 새로운 범위를 만듦
            uint16_t value = be_to_le16(write_data);
            // memcpy를 쓰면 CPU 정렬 문제를 알아서 피할 수 있음
            *(uint16_t *)addr = value;
            break;
    }
    case DATA_TYPE_32:
        {
            uint32_t value = be_to_le32(write_data);
            *(uint32_t *)addr = value;
            break;
        }
    case DATA_TYPE_64:
        {
            uint64_t value = be_to_le64(write_data);
            *(uint64_t *)addr = value;
            break;
        }
    }
}

// 차례대로 쓸 주소, data_type, 그리고 쓸 데이타가 있는 payload
static void write_memory(uint16_t addr, uint8_t data_type, uint8_t *payload) {
	// address를 마스킹함
	uint16_t addr_base = addr & 0xF00;		// 3번째 자리만 마스킹해서 타입을 구함
	uint16_t offset = addr & ~addr_base;		// 마스킹함

	// 마스킹 한 주소의 첫 필드를 매핑해둔 주소와 비교하여, 주소필드의 첫번째 주소에 더함
	switch (addr_base)
	{
	case ADDR_INPUT:		//0x100
		write_data_by_type(memory_map.input+offset, payload, data_type);
		break;
	case ADDR_OUTPUT:		//0x200
		write_data_by_type(memory_map.output+offset, payload, data_type);
		break;
	case ADDR_DATA:			//0x300
		write_data_by_type(memory_map.data+offset, payload, data_type);
		break;
	case ADDR_FLAG:			//0x400
		write_data_by_type(memory_map.flag+offset, payload, data_type);
		break;
	default:				// 이외 잘못된 주소
		return;
	}
}

// write하는 함수
static size_t execute_write(struct recived_packet *recv, uint8_t *tx_buffer) {
    uint8_t count = recv->header.count;
    uint8_t data_type = recv->header.data_type;
    uint8_t data_size = data_size_by_type(data_type);
    uint8_t *payload = recv->payload;
    
    size_t payload_offset = 0;
    
    for (int i = 0; i < count; i++) {
        uint16_t addr = be_to_le16(payload + payload_offset);
        payload_offset += 2;
        
        write_memory(addr, data_type, payload + payload_offset);
        payload_offset += data_size;
    }
    
    return build_write_ack_packet(tx_buffer, recv->header.cmd_type);  // WRITE는 cmd_type만 있으면 됨
}


/** Main program entry point. This routine configures the hardware required by the application, then
 *  enters a loop to run the application tasks in sequence.
 */
int main(void)
{
	SetupHardware();

	LEDs_SetAllLEDs(LEDMASK_USB_NOTREADY);
	GlobalInterruptEnable();

	check_result_t retval = CHECK_OK;

	struct rx_fsm rx = {
        .state = CHECK_OK,
        .expected_len = 0,
        .copied_len = 0
    };

	for (;;)
	{
		USB_USBTask();

		// 쓸 데이터 설정
		uint8_t tx_buffer[256];
		memset(&tx_buffer, 0x00, sizeof(tx_buffer));

		// 받을 데이터 설정
		uint8_t ReceivedData[VENDOR_IO_EPSIZE];
		memset(&ReceivedData, 0x00, sizeof(ReceivedData));

		Endpoint_SelectEndpoint(VENDOR_OUT_EPADDR);
		if (Endpoint_IsOUTReceived())
		{
			uint16_t rx_len = Endpoint_BytesInEndpoint();  // 실제 받은 바이트 수

			// 빈 패킷 무시
			if (rx_len == 0) {
				Endpoint_ClearOUT();
				continue;
			}
			
			Endpoint_Read_Stream_LE(ReceivedData, rx_len, NULL);	// 여기서 받은 바이트 만큼 little endian으로 읽는 것임
			Endpoint_ClearOUT();	// 엔드포인트 닫음
			
			// 첫 수신: 예상 길이 파싱 및 검증
        	if (rx.copied_len == 0) {
				if (rx_len < 2) {
					// 헤더도 안 왔음 - 에러 처리
					continue;
				}
				
				rx.expected_len = ReceivedData[1];
				
				// 길이 검증 (버퍼 크기 확인)
				if (rx.expected_len > MAX_PACKET_LEN) {
					// 에러: 버퍼 초과
					rx.copied_len = 0;  // 리셋
					continue;
				}
        	}

			// 버퍼 오버플로우 방지
			if (rx.copied_len + rx_len > MAX_PACKET_LEN) {
				// 에러 처리 후 리셋
				rx.copied_len = 0;
				rx.expected_len = 0;
				continue;
			}

			// 데이터 복사
			memcpy(rx.buf + rx.copied_len, ReceivedData, rx_len);
			rx.copied_len += rx_len;

			if (rx.copied_len >= rx.expected_len) 		
            	retval = check_packet(&rx);		// 패킷 검증
			
			if (retval != CHECK_OK) {
				uint8_t cmd_type = rx.buf[4];

				generate_error_response(tx_buffer,retval, cmd_type);
				rx.copied_len = 0;
				rx.expected_len = 0;
				send_response(tx_buffer);
				continue;
			}
			// 정상적인 패킷이면 여기서 파싱
			struct recived_packet recv;
			parse_packet(&rx, &recv);

			size_t tx_size = execute_command(&recv, tx_buffer);

			if (tx_size > 0) {
				send_response(tx_buffer);
			}

			rx.copied_len = 0;
			rx.expected_len = 0;
		}
	}
}

/** Configures the board hardware and chip peripherals for the demo's functionality. */
void SetupHardware(void)
{
#if (ARCH == ARCH_AVR8)
	/* Disable watchdog if enabled by bootloader/fuses */
	MCUSR &= ~(1 << WDRF);
	wdt_disable();

	/* Disable clock division */
	clock_prescale_set(clock_div_1);
#elif (ARCH == ARCH_XMEGA)
	/* Start the PLL to multiply the 2MHz RC oscillator to 32MHz and switch the CPU core to run from it */
	XMEGACLK_StartPLL(CLOCK_SRC_INT_RC2MHZ, 2000000, F_CPU);
	XMEGACLK_SetCPUClockSource(CLOCK_SRC_PLL);

	/* Start the 32MHz internal RC oscillator and start the DFLL to increase it to 48MHz using the USB SOF as a reference */
	XMEGACLK_StartInternalOscillator(CLOCK_SRC_INT_RC32MHZ);
	XMEGACLK_StartDFLL(CLOCK_SRC_INT_RC32MHZ, DFLL_REF_INT_USBSOF, F_USB);

	PMIC.CTRL = PMIC_LOLVLEN_bm | PMIC_MEDLVLEN_bm | PMIC_HILVLEN_bm;
#endif

	/* Hardware Initialization */
	LEDs_Init();
	USB_Init();
}

/** Event handler for the USB_Connect event. This indicates that the device is enumerating via the status LEDs. */
void EVENT_USB_Device_Connect(void)
{
	/* Indicate USB enumerating */
	LEDs_SetAllLEDs(LEDMASK_USB_ENUMERATING);
}

/** Event handler for the USB_Disconnect event. This indicates that the device is no longer connected to a host via
 *  the status LEDs.
 */
void EVENT_USB_Device_Disconnect(void)
{
	/* Indicate USB not ready */
	LEDs_SetAllLEDs(LEDMASK_USB_NOTREADY);
}

/** Event handler for the USB_ConfigurationChanged event. This is fired when the host set the current configuration
 *  of the USB device after enumeration - the device endpoints are configured.
 */
void EVENT_USB_Device_ConfigurationChanged(void)
{
	bool ConfigSuccess = true;

	/* Setup Vendor Data Endpoints */
	ConfigSuccess &= Endpoint_ConfigureEndpoint(VENDOR_IN_EPADDR,  EP_TYPE_BULK, VENDOR_IO_EPSIZE, 1);
	ConfigSuccess &= Endpoint_ConfigureEndpoint(VENDOR_OUT_EPADDR, EP_TYPE_BULK, VENDOR_IO_EPSIZE, 1);

	/* Indicate endpoint configuration success or failure */
	LEDs_SetAllLEDs(ConfigSuccess ? LEDMASK_USB_READY : LEDMASK_USB_ERROR);
}

/** Event handler for the USB_ControlRequest event. This is used to catch and process control requests sent to
 *  the device from the USB host before passing along unhandled control requests to the library for processing
 *  internally.
 */
void EVENT_USB_Device_ControlRequest(void)
{
	// Process vendor specific control requests here
}


// usb를 통해 펌웨어를 올려야할때(avrdude) 사용하는 부트로더로 진입하는 창구입니다.
void jump_to_bootloader(void) {
	
	cli();		// 이후 모든 인터럽트를 막음, USB ISR이 중간에 개입하면, WDT 전에 다른 코드가 실행될 수 있음

	// boot address key를 불러옴
	volatile uint16_t* boot_key_address = (volatile uint16_t*)BOOT_KEY_ADDR;
	
	// boot key address에 부트 키 값을 덮어씌웁니다.
	*boot_key_address = BOOT_KEY;

	// 2. LED 3번 깜빡임
    for(int i=0; i<3; i++) {
        PORTC |= (1 << PC7); // LED ON
        _delay_ms(50);
        PORTC &= ~(1 << PC7); // LED OFF
        _delay_ms(50);
    }

	USB_Disable();		// usb연결을 해제함(usb가 사라짐)

	wdt_enable(WDTO_120MS);		// 120MS후 리셋

	for(;;);			// watchdog 리셋 대기
}