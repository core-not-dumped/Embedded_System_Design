#include "migration/qemu-file.h"
#include "hw/android/goldfish/device.h"
#include "hw/android/goldfish/nand.h"
#include "hw/android/goldfish/vmem.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "android/utils/path.h"
#include "android/utils/tempfile.h"
#include "android/qemu-debug.h"
#include "android/android.h"
#include "android/skin/event.h"

extern int SDL_SendCustomEvent(int code, void* data);

#define DEV_READY	0
#define DEV_BUSY	1
#define DEV_WAIT	2

#define CMD_PUT		3
#define CMD_GET		4
#define CMD_ERASE	5
#define CMD_EXIST	6

#define BLK_NUM 	1234
#define GC_BLK_NUM 	1

int key_pos;

enum {
	/* CMD Registers */

	KVSSD_STATUS_REG	=0x00,
	KVSSD_CMD_REG		=0x04,
	KVSSD_KEY_REG		=0x08,
	KVSSD_VALUE_REG		=0x0C,
};

struct goldfish_kvssd_state {
	struct goldfish_device dev;
	int kvssd_fd_data;
	int kvssd_fd_meta;
	uint32_t status; //Ready: 0, Busy: 1
	uint32_t cmd;	//Read: 0, Write: 1
	uint16_t kvssd_key_pos;
	uint16_t kvssd_value_pos;
	uint32_t kvssd_key[4];
	uint32_t kvssd_value[1024];
	uint8_t exist;
	uint32_t blk;
	uint32_t free_blk;
};

#define KVSSD_STATE_SAVE_VERSION	3
#define QFIELD_STRUCT	struct goldfish_kvssd_state

QFIELD_BEGIN(goldfish_kvssd_fields)
	QFIELD_INT32(status),
	QFIELD_INT32(cmd),
	QFIELD_INT16(kvssd_key_pos),
	QFIELD_INT16(kvssd_value_pos),
QFIELD_END


static void goldfish_kvssd_save(QEMUFile* f, void* opaque)
{
	struct goldfish_kvssd_state* s = opaque;
	/* TODO */
	qemu_put_buffer(f, (uint8_t*)s->kvssd_key, 4 * 4);
	qemu_put_buffer(f, (uint8_t*)s->kvssd_value, 1024 * 4);
	qemu_put_struct(f, goldfish_kvssd_fields, s);
}

static int goldfish_kvssd_load(QEMUFile* f, void* opaque, int version_id)
{
	struct goldfish_kvssd_state* s = opaque;
	if ( version_id != KVSSD_STATE_SAVE_VERSION )
		return -1;
	/* TODO */
	qemu_get_buffer(f, (uint8_t*)s->kvssd_key, 4 * 4);
	qemu_get_buffer(f, (uint8_t*)s->kvssd_value, 1024 * 4);
	qemu_get_struct(f, goldfish_kvssd_fields, s);
	return 0;
}

static uint32_t goldfish_kvssd_read(void* opaque, hwaddr offset)
{
	struct goldfish_kvssd_state* s = (struct goldfish_kvssd_state*)opaque;
	uint32_t temp;

	if ( offset < 0 ) {
		cpu_abort(cpu_single_env, "kvssd_dev_read: Bad offset %" HWADDR_PRIx "\n", offset);
		return 0;
	}

	switch (offset) {
		case KVSSD_STATUS_REG:
			return s->status;
		case KVSSD_KEY_REG:
			return s->exist;
		case KVSSD_VALUE_REG:
			temp = s->kvssd_value_pos;
			s->kvssd_value_pos = (s->kvssd_value_pos+1) % 1024;
			return s->kvssd_value[temp];
	};

	return 0;
}

#define PAGE_SHIFT 12

void garbage_collection(struct goldfish_kvssd_state *s) {
	printf("gc start!\n");

	char buffer[19];
	int buffer_offset = 16 + 1 + 1;
	lseek(s->kvssd_fd_meta, 0, SEEK_SET);

	while(read(s->kvssd_fd_meta, buffer, buffer_offset))
	{
		if(buffer[16] == 'I')
		{
			buffer[16] = 'U';
			lseek(s->kvssd_fd_meta, -buffer_offset, SEEK_CUR);
			write(s->kvssd_fd_meta, buffer, buffer_offset);
			s->free_blk++;
		}
	}
	return;
}

void new_block_allocate(struct goldfish_kvssd_state *s) {
	char buffer_end[3];
	char buffer[19];
	int ind = 0;
	int buffer_offset = 16 + 1 + 1;
	lseek(s->kvssd_fd_meta, 0, SEEK_SET);
	while(read(s->kvssd_fd_meta, buffer, buffer_offset))
	{
		if(buffer[16] == 'U')
		{
			lseek(s->kvssd_fd_meta, -buffer_offset, SEEK_CUR);
			write(s->kvssd_fd_meta, s->kvssd_key, 16);
			buffer_end[0] = 'V';
			buffer_end[1] = '\n';
			write(s->kvssd_fd_meta, buffer_end, 2);
			s->blk = ind;
			s->free_blk--;
			return;
		}
		ind++;
	}

	write(s->kvssd_fd_meta, s->kvssd_key, 16);
	buffer_end[0] = 'V';
	buffer_end[1] = '\n';
	write(s->kvssd_fd_meta, buffer_end, 2);
	s->blk = ind;
	s->free_blk--;
	return;
}

static int goldfish_kvssd_data_read(struct goldfish_kvssd_state *s) {
	pread(s->kvssd_fd_data, s->kvssd_value, sizeof(s->kvssd_value), s->blk << PAGE_SHIFT);

	return 0;
}

static int goldfish_kvssd_data_write(struct goldfish_kvssd_state *s) {
	pwrite(s->kvssd_fd_data, s->kvssd_value, sizeof(s->kvssd_value), s->blk << PAGE_SHIFT);
	fsync(s->kvssd_fd_data);
	
	return 0;
}

static int check_key(struct goldfish_kvssd_state *s, int what_cmd)
{
	char buffer[19];
	int buffer_offset = 16 + 1 + 1;		// key, valid, enter
	int ind = 0;
	lseek(s->kvssd_fd_meta, 0, SEEK_SET);
	while(read(s->kvssd_fd_meta, buffer, buffer_offset))
	{
		if(buffer[16] == 'V')
		{
			uint32_t compare_num[4];
			memcpy(compare_num, buffer, 16);
			int i;
			for(i=0;i<4;i++)
			{
				if(compare_num[i] != s->kvssd_key[i])	break;
			}
			if(i == 4)
			{
				if(what_cmd == CMD_PUT || what_cmd == CMD_ERASE)
				{
					buffer[16] = 'I';
					lseek(s->kvssd_fd_meta, -buffer_offset, SEEK_CUR);
					write(s->kvssd_fd_meta, buffer, buffer_offset);
				}
				else if(what_cmd == CMD_GET)	s->blk = ind;

				s->exist = 1;
				return 1;
			}
		}
		ind++;
	}
	s->exist = 0;
	return 0;
}


static void goldfish_kvssd_write(void* opaque, hwaddr offset, uint32_t value)
{
	struct goldfish_kvssd_state* s = (struct goldfish_kvssd_state*)opaque;
	int status;

	if ( offset < 0 ) {
		cpu_abort(cpu_single_env, "kvssd_dev_read: Bad offset %" HWADDR_PRIx "\n", offset);
		return;
	}

	switch (offset) {
		case KVSSD_STATUS_REG:
			if(s->status == DEV_WAIT) {
				if(value == s->kvssd_key[key_pos]) {
					if(key_pos == 3) {
						s->status = DEV_READY;
						key_pos = 0;
					}
					else key_pos = (key_pos+1) % 4;
				}
				else {
					key_pos = 0;
					printf("error, different key\n");
				}
			}
			else	key_pos = 0;
			break;
		case KVSSD_CMD_REG:
			s->cmd = value;
			switch(s->cmd) {
				case CMD_PUT:
					if(s->free_blk <= GC_BLK_NUM)	garbage_collection(s);
					if(s->free_blk == 0)
					{
						printf("We don't have any space to put data!\n");
						cpu_abort(cpu_single_env, "kvssd_cmd: PUT error\n");
						return;
					}
					else
					{
						check_key(s, CMD_PUT);						// Check key and invalid block
						new_block_allocate(s);						// Allocate new block and change meta data
						status = goldfish_kvssd_data_write(s);		// Write 
						if ( status == 0 ) {
							goldfish_device_set_irq(&s->dev, 0, 1);
							s->status = DEV_READY;
						} else {
							cpu_abort(cpu_single_env, "kvssd_cmd: PUT error\n");
							return;
						}
					}
					break;
				case CMD_GET:
					if(check_key(s, CMD_GET))
						status = goldfish_kvssd_data_read(s);
					else	printf("Get command not accepted: Wrong key\n");

					if ( status == 0 ) {
						goldfish_device_set_irq(&s->dev, 0, 1);
						s->status = DEV_WAIT;
					} else {
						cpu_abort(cpu_single_env, "kvssd_cmd: GET error\n");
						return;
					}
					break;
				case CMD_EXIST:
					check_key(s, CMD_EXIST);
					goldfish_device_set_irq(&s->dev, 0, 1);
					s->status = DEV_WAIT;
					break;
				case CMD_ERASE:
					if(!check_key(s, CMD_ERASE))
						printf("There is no key to erase!\n");
					goldfish_device_set_irq(&s->dev, 0, 1);
					s->status = DEV_READY;
					break;
				default:
					cpu_abort(cpu_single_env, "kvssd_cmd: unsupported command %d\n", s->cmd);
					return;
			}
			break;
		case KVSSD_KEY_REG:
			s->kvssd_key[s->kvssd_key_pos] = value;
			s->kvssd_key_pos = (s->kvssd_key_pos+1) % 4;
			break;
		case KVSSD_VALUE_REG:
			s->kvssd_value[s->kvssd_value_pos] = value;
			s->kvssd_value_pos = (s->kvssd_value_pos+1) % 1024;
			break;
	};
}

static CPUReadMemoryFunc *goldfish_kvssd_readfn[] = {
	goldfish_kvssd_read,
	goldfish_kvssd_read,
	goldfish_kvssd_read
};

static CPUWriteMemoryFunc *goldfish_kvssd_writefn[] = {
	goldfish_kvssd_write,
	goldfish_kvssd_write,
	goldfish_kvssd_write
};

void goldfish_kvssd_init(void)
{
	struct goldfish_kvssd_state *s;

	s = (struct goldfish_kvssd_state *)g_malloc0(sizeof(*s));
	s->kvssd_key_pos = 0;
	s->kvssd_value_pos = 0;
	s->dev.name = "goldfish_kvssd";
	s->dev.base = 0;
	s->dev.size = 0x1000;
	s->dev.irq_count = 1;
	s->dev.irq = 15;
	//Change to absolute path & Add authority(0777)
	s->kvssd_fd_data = open("/home/lee/emu-2.2-release/external/qemu/kvssd.disk", O_RDWR | O_CREAT, 0777);
	s->kvssd_fd_meta = open("/home/lee/emu-2.2-release/external/qemu/kvssd.meta", O_RDWR | O_CREAT, 0777);
	s->free_blk = BLK_NUM;
	key_pos = 0;

	char buffer[19];
	int buffer_offset = 16 + 1 + 1;
	lseek(s->kvssd_fd_meta, 0, SEEK_SET);
	while(read(s->kvssd_fd_meta, buffer, buffer_offset))
	{
		if(buffer[16] != 'U')
			s->free_blk--;
	}
	if(s->free_blk <= GC_BLK_NUM)	garbage_collection(s);


	goldfish_device_add(&s->dev, goldfish_kvssd_readfn, goldfish_kvssd_writefn, s);

	register_savevm(NULL, 
			"goldfish_kvssd",
			0,
			KVSSD_STATE_SAVE_VERSION,
			goldfish_kvssd_save,
			goldfish_kvssd_load,
			s);
}
