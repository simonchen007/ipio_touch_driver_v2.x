/*
 * ILITEK Touch IC driver
 *
 * Copyright (C) 2011 ILI Technology Corporation.
 *
 * Author: Dicky Chiang <dicky_chiang@ilitek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
#include "ilitek.h"
/* Firmware data with static array */
#include "ilitek_fw.h"

#define UPDATE_PASS		0
#define UPDATE_FAIL		-1
#define TIMEOUT_SECTOR		500
#define TIMEOUT_PAGE		3500
#define TIMEOUT_PROGRAM		10

struct touch_fw_data {
	u8 block_number;
	u32 start_addr;
	u32 end_addr;
	u32 new_fw_cb;
	int delay_after_upgrade;
	bool isCRC;
	bool isboot;
	int hex_tag;
} tfd;

struct flash_block_info {
	char *name;
	u32 start;
	u32 end;
	u32 len;
	u32 mem_start;
	u32 fix_mem_start;
	u8 mode;
} fbi[FW_BLOCK_INFO_NUM];

u8 gestrue_fw[(10 * K)];

static u32 HexToDec(char *phex, s32 len)
{
	u32 ret = 0, temp = 0, i;
	s32 shift = (len - 1) * 4;

	for (i = 0; i < len; shift -= 4, i++) {
		if ((phex[i] >= '0') && (phex[i] <= '9'))
			temp = phex[i] - '0';
		else if ((phex[i] >= 'a') && (phex[i] <= 'f'))
			temp = (phex[i] - 'a') + 10;
		else if ((phex[i] >= 'A') && (phex[i] <= 'F'))
			temp = (phex[i] - 'A') + 10;
		else
			return -1;

		ret |= (temp << shift);
	}
	return ret;
}

static int CalculateCRC32(u32 start_addr, u32 len, u8 *pfw)
{
	int i = 0, j = 0;
	int crc_poly = 0x04C11DB7;
	int tmp_crc = 0xFFFFFFFF;

	for (i = start_addr; i < start_addr + len; i++) {
		tmp_crc ^= (pfw[i] << 24);

		for (j = 0; j < 8; j++) {
			if ((tmp_crc & 0x80000000) != 0)
				tmp_crc = tmp_crc << 1 ^ crc_poly;
			else
				tmp_crc = tmp_crc << 1;
		}
	}
	return tmp_crc;
}

static int host_download_dma_check(u32 start_addr, u32 block_size)
{
	int count = 50;
	u32 busy = 0;

	/* dma1 src1 address */
	if (ilitek_ice_mode_write(0x072104, start_addr, 4) < 0)
		ipio_err("Write dma1 src1 address failed\n");
	/* dma1 src1 format */
	if (ilitek_ice_mode_write(0x072108, 0x80000001, 4) < 0)
		ipio_err("Write dma1 src1 format failed\n");
	/* dma1 dest address */
	if (ilitek_ice_mode_write(0x072114, 0x00030000, 4) < 0)
		ipio_err("Write dma1 src1 format failed\n");
	/* dma1 dest format */
	if (ilitek_ice_mode_write(0x072118, 0x80000000, 4) < 0)
		ipio_err("Write dma1 dest format failed\n");
	/* Block size*/
	if (ilitek_ice_mode_write(0x07211C, block_size, 4) < 0)
		ipio_err("Write block size (%d) failed\n", block_size);

	idev->chip->hd_dma_check_crc_off();

	/* crc on */
	if (ilitek_ice_mode_write(0x041016, 0x01, 1) < 0)
		ipio_err("Write crc on failed\n");
	/* Dma1 stop */
	if (ilitek_ice_mode_write(0x072100, 0x00000000, 4) < 0)
		ipio_err("Write dma1 stop failed\n");
	/* clr int */
	if (ilitek_ice_mode_write(0x048006, 0x1, 1) < 0)
		ipio_err("Write clr int failed\n");
	/* Dma1 start */
	if (ilitek_ice_mode_write(0x072100, 0x01000000, 4) < 0)
		ipio_err("Write dma1 start failed\n");

	/* Polling BIT0 */
	while (count > 0) {
		mdelay(1);
		if (ilitek_ice_mode_read(0x048006, &busy, sizeof(u8)) < 0)
			ipio_err("Read busy error\n");
		ipio_debug("busy = %x\n", busy);
		if ((busy & 0x01) == 1)
			break;
		count--;
	}

	if (count <= 0) {
		ipio_err("BIT0 is busy\n");
		return -1;
	}

	if (ilitek_ice_mode_read(0x04101C, &busy, sizeof(u32)) < 0) {
		ipio_err("Read dma crc error\n");
		return -1;
	}
	return busy;
}

static int ilitek_tddi_fw_iram_read(u8 *buf, u32 start, u32 end)
{
	int r_len = end + 1;
	u8 cmd[4] = {0};

	if (!buf) {
		ipio_err("buf in null\n");
		return -ENOMEM;
	}

	cmd[0] = 0x25;
	cmd[3] = (char)((start & 0x00FF0000) >> 16);
	cmd[2] = (char)((start & 0x0000FF00) >> 8);
	cmd[1] = (char)((start & 0x000000FF));

	if (idev->write(cmd, 4)) {
		ipio_err("Failed to write iram data\n");
		return -ENODEV;
	}

	if (idev->read(buf, r_len)) {
		ipio_err("Failed to Read iram data\n");
		return -ENODEV;
	}
	return 0;
}

void ilitek_fw_dump_iram_data(u32 start, u32 end, bool save)
{
	struct file *f = NULL;
	mm_segment_t old_fs;
	loff_t pos = 0;
	int wdt, i;
	int len, tmp = ipio_debug_level;
	bool ice = atomic_read(&idev->ice_stat);

	if (!ice) {
		if (ilitek_ice_mode_ctrl(ENABLE, OFF) < 0) {
			ipio_err("Enable ice mode failed\n");
			return;
		}
	}

	wdt = ilitek_tddi_ic_watch_dog_ctrl(ILI_READ, DISABLE);
	if (wdt) {
		if (ilitek_tddi_ic_watch_dog_ctrl(ILI_WRITE, DISABLE) < 0)
			ipio_err("Disable WDT failed during dumping iram\n");
	}

	len = end - start + 1;

	for (i = 0; i < len; i++)
		idev->fw_dma_buf[i] = 0xFF;

	if (ilitek_tddi_fw_iram_read(idev->fw_dma_buf, start, end) < 0)
		ipio_err("Read IRAM data failed\n");

	if (save) {
		f = filp_open(DUMP_IRAM_PATH, O_WRONLY | O_CREAT | O_TRUNC, 644);
		if (ERR_ALLOC_MEM(f)) {
			ipio_err("Failed to open the file at %ld.\n", PTR_ERR(f));
			goto out;
		}

		old_fs = get_fs();
		set_fs(get_ds());
		set_fs(KERNEL_DS);
		pos = 0;
		vfs_write(f, idev->fw_dma_buf, len, &pos);
		set_fs(old_fs);
		filp_close(f, NULL);
		ipio_info("Save iram data to %s\n", DUMP_IRAM_PATH);
	} else {
		ipio_debug_level = DEBUG_ALL;
		ilitek_dump_data(idev->fw_dma_buf, 8, len, 0, "IRAM");
		ipio_debug_level = tmp;
	}

out:
	if (wdt) {
		if (ilitek_tddi_ic_watch_dog_ctrl(ILI_WRITE, ENABLE) < 0)
			ipio_err("Enable WDT failed during dumping iram\n");
	}

	if (!ice) {
		if (ilitek_ice_mode_ctrl(DISABLE, OFF) < 0)
			ipio_err("Enable ice mode failed after code reset\n");
	}

	ipio_info("dump iram data completed\n");
}

static int ilitek_tddi_fw_iram_program(u32 start, u8 *w_buf, u32 w_len, u32 split_len)
{
	int i = 0, j = 0, addr = 0;
	u32 end = start + w_len;

	for (i = 0; i < MAX_HEX_FILE_SIZE; i++)
		idev->fw_dma_buf[i] = 0xFF;

	if (split_len != 0) {
		for (addr = start, i = 0; addr < end; addr += split_len, i += split_len) {
			if ((addr + split_len) > end)
				split_len = end - addr;

			idev->fw_dma_buf[0] = SPI_WRITE;
			idev->fw_dma_buf[1] = 0x25;
			idev->fw_dma_buf[2] = (char)((addr & 0x000000FF));
			idev->fw_dma_buf[3] = (char)((addr & 0x0000FF00) >> 8);
			idev->fw_dma_buf[4] = (char)((addr & 0x00FF0000) >> 16);

			for (j = 0; j < split_len; j++)
				idev->fw_dma_buf[5 + j] = w_buf[i + j];

			if (idev->spi_write_then_read(idev->spi, idev->fw_dma_buf, split_len + 5, NULL, 0)) {
				ipio_err("Failed to write data via SPI in host download (%x)\n", split_len + 5);
				return -EIO;
			}
		}
	} else {
		idev->fw_dma_buf[0] = SPI_WRITE;
		idev->fw_dma_buf[1] = 0x25;
		idev->fw_dma_buf[2] = (char)((start & 0x000000FF));
		idev->fw_dma_buf[3] = (char)((start & 0x0000FF00) >> 8);
		idev->fw_dma_buf[4] = (char)((start & 0x00FF0000) >> 16);

		memcpy(&idev->fw_dma_buf[5], w_buf, w_len);

		/* It must be supported by platforms that have the ability to transfer all data at once. */
		if (idev->spi_write_then_read(idev->spi, idev->fw_dma_buf, w_len + 5, NULL, 0) < 0) {
			ipio_err("Failed to write data via SPI in host download (%x)\n", w_len + 5);
			return -EIO;
		}
	}
	return 0;
}

static int ilitek_tddi_fw_iram_upgrade(u8 *pfw)
{
	int i, ret = UPDATE_PASS, size;
	u32 mode, crc, dma;
	u8 *fw_ptr = NULL;

	if (!idev->ddi_rest_done) {
		if (idev->actual_tp_mode != P5_X_FW_GESTURE_MODE)
			ilitek_tddi_reset_ctrl(idev->reset);

		ret = ilitek_ice_mode_ctrl(ENABLE, OFF);
		if (ret < 0)
			return ret;
	} else {
		/* Restore it if the wq of load_fw_ddi has been called. */
		idev->ddi_rest_done = false;
	}

	ret = ilitek_tddi_ic_watch_dog_ctrl(ILI_WRITE, DISABLE);
	if (ret < 0)
		return ret;

	fw_ptr = pfw;
	if (idev->actual_tp_mode == P5_X_FW_TEST_MODE) {
		mode = MP;
	} else if (idev->actual_tp_mode == P5_X_FW_GESTURE_MODE) {
		mode = GESTURE;
		fw_ptr = gestrue_fw;
	} else {
		mode = AP;
	}

	/* Program data to iram acorrding to each block */
	size = ARRAY_SIZE(fbi);
	for (i = 0; i < size; i++) {
		if (fbi[i].mode == mode && fbi[i].len != 0) {
			ipio_info("Download %s code from hex 0x%x to IRAM 0x%x, len = 0x%x\n",
					fbi[i].name, fbi[i].start, fbi[i].mem_start, fbi[i].len);

#if SPI_DMA_TRANSFER_SPLIT
			if (ilitek_tddi_fw_iram_program(fbi[i].mem_start, (fw_ptr + fbi[i].start), fbi[i].len, SPI_UPGRADE_LEN) < 0)
				ipio_err("IRAM program failed\n");
#else
			if (ilitek_tddi_fw_iram_program(fbi[i].mem_start, (fw_ptr + fbi[i].start), fbi[i].len, 0) < 0)
				ipio_err("IRAM program failed\n");
#endif

			crc = CalculateCRC32(fbi[i].start, fbi[i].len - 4, fw_ptr);
			dma = host_download_dma_check(fbi[i].mem_start, fbi[i].len - 4);

			ipio_info("%s CRC is %s (%x) : (%x)\n",
				fbi[i].name, (crc != dma ? "Invalid !" : "Correct !"), crc, dma);

			if (crc != dma) {
				ipio_err("CRC Failed! print iram 64k bytes for debug\n");
				ilitek_fw_dump_iram_data(0x0, 0xFFFF, false);
				return UPDATE_FAIL;
			}
			idev->fw_update_stat = 90;
		}
	}

	if (idev->actual_tp_mode != P5_X_FW_GESTURE_MODE) {
		if (ilitek_tddi_reset_ctrl(TP_IC_CODE_RST) < 0)
			ipio_err("TP Code reset failed during iram programming\n");
	}

	if (ilitek_ice_mode_ctrl(DISABLE, OFF) < 0)
		ipio_err("Disable ice mode failed after code reset\n");

	/* Waiting for fw ready sending first cmd */
	if (!idev->info_from_hex)
		mdelay(100);
	return ret;
}

static void ilitek_tddi_fw_update_block_info(u8 *pfw)
{
	u32 ges_area_section, ges_info_addr, ges_fw_start, ges_fw_end;
	u32 ap_end, ap_len;

	ipio_info("Tag = %x\n", tfd.hex_tag);

        if (tfd.hex_tag == BLOCK_TAG_AF) {
                fbi[AP].mem_start = (fbi[AP].fix_mem_start != INT_MAX) ? fbi[AP].fix_mem_start : 0;
                fbi[DATA].mem_start = (fbi[DATA].fix_mem_start != INT_MAX) ? fbi[DATA].fix_mem_start : DLM_START_ADDRESS;
                fbi[TUNING].mem_start = (fbi[TUNING].fix_mem_start != INT_MAX) ? fbi[TUNING].fix_mem_start :  fbi[DATA].mem_start + fbi[DATA].len;
                fbi[MP].mem_start = (fbi[MP].fix_mem_start != INT_MAX) ? fbi[MP].fix_mem_start :  0;
                fbi[GESTURE].mem_start = (fbi[GESTURE].fix_mem_start != INT_MAX) ? fbi[GESTURE].fix_mem_start :	 0;

                /* Parsing gesture info form AP code */
                ges_info_addr = (fbi[AP].end + 1 - 60);
                ges_area_section = (pfw[ges_info_addr + 3] << 24) + (pfw[ges_info_addr + 2] << 16) + (pfw[ges_info_addr + 1] << 8) + pfw[ges_info_addr];
                fbi[GESTURE].mem_start = (pfw[ges_info_addr + 7] << 24) + (pfw[ges_info_addr + 6] << 16) + (pfw[ges_info_addr + 5] << 8) + pfw[ges_info_addr + 4];
                ap_end = (pfw[ges_info_addr + 11] << 24) + (pfw[ges_info_addr + 10] << 16) + (pfw[ges_info_addr + 9] << 8) + pfw[ges_info_addr + 8];
                if (ap_end != fbi[GESTURE].mem_start)
                        ap_len = ap_end - fbi[GESTURE].mem_start + 1;
                ges_fw_start = (pfw[ges_info_addr + 15] << 24) + (pfw[ges_info_addr + 14] << 16) + (pfw[ges_info_addr + 13] << 8) + pfw[ges_info_addr + 12];
                ges_fw_end = (pfw[ges_info_addr + 19] << 24) + (pfw[ges_info_addr + 18] << 16) + (pfw[ges_info_addr + 17] << 8) + pfw[ges_info_addr + 16];
                if (ges_fw_end != ges_fw_start)
                        fbi[GESTURE].len = ges_fw_end - ges_fw_start + 1;
                fbi[GESTURE].start = 0;

        } else {
                memset(fbi, 0x0, sizeof(fbi));
                fbi[AP].start = 0;
                fbi[AP].mem_start = 0;
                fbi[AP].len = MAX_AP_FIRMWARE_SIZE;

                fbi[DATA].start = DLM_HEX_ADDRESS;
                fbi[DATA].mem_start = DLM_START_ADDRESS;
                fbi[DATA].len = MAX_DLM_FIRMWARE_SIZE;

                fbi[MP].start = MP_HEX_ADDRESS;
                fbi[MP].mem_start = 0;
                fbi[MP].len = MAX_MP_FIRMWARE_SIZE;

                /* Parsing gesture info form AP code */
                ges_info_addr = (MAX_AP_FIRMWARE_SIZE - 60);
                ges_area_section = (pfw[ges_info_addr + 3] << 24) + (pfw[ges_info_addr + 2] << 16) + (pfw[ges_info_addr + 1] << 8) + pfw[ges_info_addr];
                fbi[GESTURE].mem_start = (pfw[ges_info_addr + 7] << 24) + (pfw[ges_info_addr + 6] << 16) + (pfw[ges_info_addr + 5] << 8) + pfw[ges_info_addr + 4];
                ap_end = (pfw[ges_info_addr + 11] << 24) + (pfw[ges_info_addr + 10] << 16) + (pfw[ges_info_addr + 9] << 8) + pfw[ges_info_addr + 8];
                if (ap_end != fbi[GESTURE].mem_start)
                        ap_len = ap_end - fbi[GESTURE].mem_start + 1;
                ges_fw_start = (pfw[ges_info_addr + 15] << 24) + (pfw[ges_info_addr + 14] << 16) + (pfw[ges_info_addr + 13] << 8) + pfw[ges_info_addr + 12];
                ges_fw_end = (pfw[ges_info_addr + 19] << 24) + (pfw[ges_info_addr + 18] << 16) + (pfw[ges_info_addr + 17] << 8) + pfw[ges_info_addr + 16];
                if (ges_fw_end != ges_fw_start)
                        fbi[GESTURE].len = ges_fw_end - ges_fw_start + 1;
                fbi[GESTURE].start = 0;

        }

        memset(gestrue_fw, 0xff, sizeof(gestrue_fw));

        /* Copy gesture data */
        if (fbi[GESTURE].mem_start != 0xffffffff && ges_fw_start != 0xffffffff && fbi[GESTURE].mem_start != 0 && ges_fw_start != 0)
                ipio_memcpy(gestrue_fw, (pfw + ges_fw_start), fbi[GESTURE].len, sizeof(gestrue_fw));
        else
                ipio_err("There is no gesture data inside fw\n");

        ipio_info("==== Gesture loader info ====\n");
        ipio_info("ap_start = 0x%x, ap_end = 0x%x, ap_len = 0x%x\n", fbi[GESTURE].mem_start, ap_end, ap_len);
        ipio_info("gesture_start = 0x%x, gesture_end = 0x%x, gesture_len = 0x%x\n", ges_fw_start, ges_fw_end, fbi[GESTURE].len);
        ipio_info("=============================\n");

        fbi[AP].name = "AP";
        fbi[DATA].name = "DATA";
        fbi[TUNING].name = "TUNING";
        fbi[MP].name = "MP";
        fbi[GESTURE].name = "GESTURE";

        /* upgrade mode define */
        fbi[DATA].mode = fbi[AP].mode = fbi[TUNING].mode = AP;
        fbi[MP].mode = MP;
        fbi[GESTURE].mode = GESTURE;

	/* Save fw info buffer */
	ipio_memcpy(idev->chip->info, (pfw + idev->chip->info_addr), sizeof(idev->chip->info), sizeof(idev->chip->info));

	/* Get hex fw vers */
	tfd.new_fw_cb = (pfw[FW_VER_ADDR] << 24) | (pfw[FW_VER_ADDR + 1] << 16) |
			(pfw[FW_VER_ADDR + 2] << 8) | (pfw[FW_VER_ADDR + 3]);

	/* Calculate update address */
	ipio_info("New FW ver = 0x%x\n", tfd.new_fw_cb);
	ipio_info("star_addr = 0x%06X, end_addr = 0x%06X, Block Num = %d\n", tfd.start_addr, tfd.end_addr, tfd.block_number);
}

static int ilitek_tddi_fw_ili_convert(u8 *pfw)
{
	int i = 0, block_enable = 0, num = 0;
	u8 block;
	u32 Addr;

	ipio_info("Start to parse ILI file, type = %d, block_count = %d\n", CTPM_FW[32], CTPM_FW[33]);

	memset(fbi, 0x0, sizeof(fbi));

	tfd.start_addr = 0;
	tfd.end_addr = 0;
	tfd.hex_tag = 0;

	block_enable = CTPM_FW[32];

	if (block_enable == 0) {
		tfd.hex_tag = BLOCK_TAG_AE;
		goto out;
	}

	tfd.hex_tag = BLOCK_TAG_AF;
	for (i = 0; i < FW_BLOCK_INFO_NUM; i++) {
		if (((block_enable >> i) & 0x01) == 0x01) {
			num = i + 1;

			if (num > (FW_BLOCK_INFO_NUM - 1)) {
				ipio_err("ERROR! block num is larger than its define (%d, %d)\n",
						num, FW_BLOCK_INFO_NUM - 1);
				return -EINVAL;
			}

			if ((num) == 6) {
				fbi[num].start = (CTPM_FW[0] << 16) + (CTPM_FW[1] << 8) + (CTPM_FW[2]);
				fbi[num].end = (CTPM_FW[3] << 16) + (CTPM_FW[4] << 8) + (CTPM_FW[5]);
				fbi[num].fix_mem_start = INT_MAX;
			} else {
				fbi[num].start = (CTPM_FW[34 + i * 6] << 16) + (CTPM_FW[35 + i * 6] << 8) + (CTPM_FW[36 + i * 6]);
				fbi[num].end = (CTPM_FW[37 + i * 6] << 16) + (CTPM_FW[38 + i * 6] << 8) + (CTPM_FW[39 + i * 6]);
				fbi[num].fix_mem_start = INT_MAX;
			}

			if (fbi[num].start == fbi[num].end)
				continue;
			fbi[num].len = fbi[num].end - fbi[num].start + 1;
			ipio_info("Block[%d]: start_addr = %x, end = %x\n", num, fbi[num].start, fbi[num].end);

			if (num == GESTURE) {
				ipio_info("ili file has gesture block\n");
				idev->gesture_load_code = true;
			}
		}
	}

	if ((block_enable & 0x80) == 0x80) {
		for (i = 0; i < 3; i++) {
			Addr = (CTPM_FW[6 + i * 4] << 16) + (CTPM_FW[7 + i * 4] << 8) + (CTPM_FW[8 + i * 4]);
			block = CTPM_FW[9 + i * 4];

			if ((block != 0) && (Addr != 0x000000)) {
				fbi[block].fix_mem_start = Addr;
				ipio_info("Tag 0xB0: change Block[%d] to addr = 0x%x\n", block, fbi[block].fix_mem_start);
			}
		}
	}

out:
	tfd.block_number = CTPM_FW[33];
	memcpy(pfw, CTPM_FW + ILI_FILE_HEADER, (sizeof(CTPM_FW) - ILI_FILE_HEADER));
	tfd.end_addr = (sizeof(CTPM_FW) - ILI_FILE_HEADER);
	return 0;
}

static int ilitek_tddi_fw_hex_convert(u8 *phex, int size, u8 *pfw)
{
	int block = 0, array_size;
	u32 i = 0, j = 0, k = 0, num = 0;
	u32 len = 0, addr = 0, type = 0;
	u32 start_addr = 0x0, end_addr = 0x0, ex_addr = 0;
	u32 offset, hex_crc, data_crc;

	memset(fbi, 0x0, sizeof(fbi));

	/* Parsing HEX file */
	for (; i < size;) {
		len = HexToDec(&phex[i + 1], 2);
		addr = HexToDec(&phex[i + 3], 4);
		type = HexToDec(&phex[i + 7], 2);

		if (type == 0x04) {
			ex_addr = HexToDec(&phex[i + 9], 4);
		} else if (type == 0x02) {
			ex_addr = HexToDec(&phex[i + 9], 4);
			ex_addr = ex_addr >> 12;
		} else if (type == BLOCK_TAG_AE || type == BLOCK_TAG_AF) {
			/* insert block info extracted from hex */
			tfd.hex_tag = type;
			if (tfd.hex_tag == BLOCK_TAG_AF)
				num = HexToDec(&phex[i + 9 + 6 + 6], 2);
			else
				num = block;

			if (num > (FW_BLOCK_INFO_NUM - 1)) {
				ipio_err("ERROR! block num is larger than its define (%d, %d)\n",
						num, FW_BLOCK_INFO_NUM - 1);
				return -EINVAL;
			}

			fbi[num].start = HexToDec(&phex[i + 9], 6);
			fbi[num].end = HexToDec(&phex[i + 9 + 6], 6);
			fbi[num].fix_mem_start = INT_MAX;
			fbi[num].len = fbi[num].end - fbi[num].start + 1;
			ipio_info("Block[%d]: start_addr = %x, end = %x", num, fbi[num].start, fbi[num].end);
			if (num == GESTURE) {
				ipio_info("hex file has gesture block\n");
				idev->gesture_load_code = true;
			}

			block++;
		} else if (type == BLOCK_TAG_B0 && tfd.hex_tag == BLOCK_TAG_AF) {
			num = HexToDec(&phex[i + 9 + 6], 2);

			if (num > (FW_BLOCK_INFO_NUM - 1)) {
				ipio_err("ERROR! block num is larger than its define (%d, %d)\n",
						num, FW_BLOCK_INFO_NUM - 1);
				return -EINVAL;
			}

			fbi[num].fix_mem_start = HexToDec(&phex[i + 9], 6);
			ipio_info("Tag 0xB0: change Block[%d] to addr = 0x%x\n", num, fbi[num].fix_mem_start);
		}

		addr = addr + (ex_addr << 16);

		if (phex[i + 1 + 2 + 4 + 2 + (len * 2) + 2] == 0x0D)
			offset = 2;
		else
			offset = 1;

		if (addr > MAX_HEX_FILE_SIZE) {
			ipio_err("Invalid hex format %d\n", addr);
			return -1;
		}

		if (type == 0x00) {
			end_addr = addr + len;
			if (addr < start_addr)
				start_addr = addr;
			/* fill data */
			for (j = 0, k = 0; j < (len * 2); j += 2, k++)
				pfw[addr + k] = HexToDec(&phex[i + 9 + j], 2);
		}
		i += 1 + 2 + 4 + 2 + (len * 2) + 2 + offset;
	}

	/* Check the content of hex file by comparsing parsed data to the crc at last 4 bytes */
	array_size = ARRAY_SIZE(fbi);
	for (i = 0; i < array_size; i++) {
		if (fbi[i].end == 0)
			continue;
		ex_addr = fbi[i].end;
		data_crc = CalculateCRC32(fbi[i].start, fbi[i].len - 4, pfw);
		hex_crc = pfw[ex_addr - 3] << 24 | pfw[ex_addr - 2] << 16 | pfw[ex_addr - 1] << 8 | pfw[ex_addr];
		ipio_debug("data crc = %x, hex crc = %x\n", data_crc, hex_crc);
		if (data_crc != hex_crc) {
			ipio_err("Content of hex file is broken. (%d, %x, %x)\n",
				i, data_crc, hex_crc);
			return -1;
		}
	}

	ipio_info("Contect of hex file is correct\n");
	tfd.start_addr = start_addr;
	tfd.end_addr = end_addr;
	tfd.block_number = block;
	return 0;
}

static int ilitek_tdd_fw_hex_open(u8 open_file_method, u8 *pfw)
{
	int fsize = 0;
	const struct firmware *fw = NULL;
	struct file *f = NULL;
	mm_segment_t old_fs;
	loff_t pos = 0;

	ipio_info("Open file method = %s, path = %s\n",
		open_file_method ? "FILP_OPEN" : "REQUEST_FIRMWARE",
		open_file_method ? UPDATE_FW_FILP_PATH : UPDATE_FW_REQUEST_PATH);

	switch (open_file_method) {
	case REQUEST_FIRMWARE:
		if (request_firmware(&fw, UPDATE_FW_REQUEST_PATH, idev->dev) < 0) {
			ipio_err("Request firmware failed\n");
			goto convert_hex;
		}

		fsize = fw->size;
		ipio_info("fsize = %d\n", fsize);
		if (fsize <= 0) {
			ipio_err("The size of file is zero\n");
			release_firmware(fw);
			goto convert_hex;
		}

		ipio_vfree((void **) & (idev->tp_fw.data));
		idev->tp_fw.size = 0;
		idev->tp_fw.data = vmalloc(fsize);
		if (!idev->tp_fw.data) {
			ipio_err("Failed to allocate tp_fw by vmalloc, try again\n");
			idev->tp_fw.data = vmalloc(fsize);
			if (!idev->tp_fw.data) {
				ipio_err("Failed to allocate tp_fw after retry\n");
				return -ENOMEM;
			}
		}

		/* Copy fw data got from request_firmware to global */
		ipio_memcpy((u8 *)idev->tp_fw.data, fw->data, fsize * sizeof(*fw->data), fsize);
		idev->tp_fw.size = fsize;
		release_firmware(fw);
		break;
	case FILP_OPEN:
		f = filp_open(UPDATE_FW_FILP_PATH, O_RDONLY, 0644);
		if (ERR_ALLOC_MEM(f)) {
			ipio_err("Failed to open the file at %ld, try to load it from tp_fw\n", PTR_ERR(f));
			goto convert_hex;
		}

		fsize = f->f_inode->i_size;
		ipio_info("fsize = %d\n", fsize);
		if (fsize <= 0) {
			ipio_err("The size of file is invaild, try to load it from tp_fw\n");
			filp_close(f, NULL);
			goto convert_hex;
		}

		ipio_vfree((void **) & (idev->tp_fw.data));
		idev->tp_fw.size = 0;
		idev->tp_fw.data = vmalloc(fsize);
		if (idev->tp_fw.data == NULL) {
			ipio_err("Failed to allocate tp_fw by vmalloc, try again\n");
			idev->tp_fw.data = vmalloc(fsize);
			if (idev->tp_fw.data == NULL) {
				ipio_err("Failed to allocate tp_fw after retry\n");
				filp_close(f, NULL);
				return -ENOMEM;
			}
		}

		/* ready to map user's memory to obtain data by reading files */
		old_fs = get_fs();
		set_fs(get_ds());
		set_fs(KERNEL_DS);
		pos = 0;
		vfs_read(f, (u8 *)idev->tp_fw.data, fsize, &pos);
		set_fs(old_fs);
		filp_close(f, NULL);
		idev->tp_fw.size = fsize;
		break;
	default:
		ipio_err("Unknown open file method, %d\n", open_file_method);
		break;
	}

convert_hex:
	/* Convert hex and copy data from tp_fw.data to pfw */
	if (!ERR_ALLOC_MEM(idev->tp_fw.data) && idev->tp_fw.size > 0) {
		if (ilitek_tddi_fw_hex_convert((u8 *)idev->tp_fw.data, idev->tp_fw.size, pfw) < 0) {
			ipio_err("Convert hex file failed\n");
			return -1;
		}
	} else {
		ipio_err("tp_fw is NULL or fsize(%d) is zero\n", fsize);
		return -1;
	}
	return 0;
}

static void ilitek_tddi_fw_check_update(int ret)
{
	ipio_info("FW upgrade %s\n", (ret == UPDATE_PASS ? "PASS" : "FAIL"));

        if (ret == UPDATE_PASS)
                return;

        if (atomic_read(&idev->mp_stat)) {
                ipio_info("No need to erase data during mp test\n");
                return;
        }

        ipio_info("Erase all fw data\n");

        if (ilitek_tddi_reset_ctrl(idev->reset) < 0)
                ipio_err("TP reset failed while erasing data\n");
}

int ilitek_tddi_fw_upgrade(int file_type, int open_file_method)
{
	int ret = 0, retry = 3;
	u8 *pfw = NULL;

	pfw = vmalloc(MAX_HEX_FILE_SIZE * sizeof(u8));
	if (ERR_ALLOC_MEM(pfw)) {
		ipio_err("Failed to allocate pfw memory, %ld\n", PTR_ERR(pfw));
		ret = -ENOMEM;
		goto out;
	}

	idev->gesture_load_code = false;
	memset(pfw, 0xFF, MAX_HEX_FILE_SIZE * sizeof(u8));

	ipio_info("Convert FW file from %s\n", (file_type == ILI_FILE ? "ILI_FILE" : "HEX_FILE"));

	if (idev->actual_tp_mode != P5_X_FW_GESTURE_MODE) {
		if (ilitek_tdd_fw_hex_open(open_file_method, pfw) < 0) {
			ipio_err("Open hex file fail, try upgrade from ILI file\n");
			if (ilitek_tddi_fw_ili_convert(pfw) < 0) {
				ipio_err("Convert ILI file error\n");
				ret = UPDATE_FAIL;
				goto out;
			}
		}
		ilitek_tddi_fw_update_block_info(pfw);
	}

	do {
		ret = ilitek_tddi_fw_iram_upgrade(pfw);
		if (ret == UPDATE_PASS)
			break;

		ipio_err("Upgrade failed, do retry!\n");
	} while (--retry > 0);

	if (ret != UPDATE_PASS) {
		ipio_err("Upgrade firmware failed after retry 3 times\n");
		ret = UPDATE_FAIL;
	}

out:
	ilitek_tddi_fw_check_update(ret);
	ipio_vfree((void **)&pfw);
	return ret;
}

void ilitek_tddi_fw_read_flash_info()
{
        return;
}

void ilitek_tddi_flash_clear_dma(void)
{
        return;
}

void ilitek_tddi_flash_dma_write(u32 start, u32 end, u32 len)
{
        return;
}

int ilitek_tddi_fw_dump_flash_data(u32 start, u32 end, bool user)
{
        return 0;
}