; Firmware loader from cartridge
; (C) 2023-2025 by Diego Parrilla
; License: GPL v3

; Some technical info about the header format https://www.atari-forum.com/viewtopic.php?t=14086

; $FA0000 - CA_MAGIC. Magic number, always $abcdef42 for ROM cartridge. There is a special magic number for testing: $fa52235f.
; $FA0004 - CA_NEXT. Address of next program in cartridge, or 0 if no more.
; $FA0008 - CA_INIT. Address of optional init. routine. See below for details.
; $FA000C - CA_RUN. Address of program start. All optional inits are done before. This is required only if program runs under GEMDOS.
; $FA0010 - CA_TIME. File's time stamp. In GEMDOS format.
; $FA0012 - CA_DATE. File's date stamp. In GEMDOS format.
; $FA0014 - CA_SIZE. Lenght of app. in bytes. Not really used.
; $FA0018 - CA_NAME. DOS/TOS filename 8.3 format. Terminated with 0 .

; CA_INIT holds address of optional init. routine. Bits 24-31 aren't used for addressing, and ensure in which moment by system init prg. will be initialized and/or started. Bits have following meanings, 1 means execution:
; bit 24: Init. or start of cartridge SW after succesfull HW init. System variables and vectors are set, screen is set, Interrupts are disabled - level 7.
; bit 25: As by bit 24, but right after enabling interrupts on level 3. Before GEMDOS init.
; bit 26: System init is done until setting screen resolution. Otherwise as bit 24.
; bit 27: After GEMDOS init. Before booting from disks.
; bit 28: -
; bit 29: Program is desktop accessory - ACC .	 
; bit 30: TOS application .
; bit 31: TTP

ROM4_ADDR			equ $FA0000
SCREEN_SIZE			equ (-4096)	; Use the memory before the screen memory to store the copied code

; Constants needed for the commands
RANDOM_TOKEN_ADDR:        equ (ROM4_ADDR + $F000) 	      ; Random token address at $FAF000
RANDOM_TOKEN_SEED_ADDR:   equ (RANDOM_TOKEN_ADDR + 4) 	  ; RANDOM_TOKEN_ADDR + 4 bytes
RANDOM_TOKEN_POST_WAIT:   equ $1        		      	  ; Wait this cycles after the random number generator is ready
COMMAND_TIMEOUT           equ $0000FFFF                   ; Timeout for the command

SHARED_VARIABLES:     	  equ (RANDOM_TOKEN_ADDR + $200)  ; random token + 512 bytes to the shared variables area: $FAF200

ROMCMD_START_ADDR:        equ $FB0000					  ; We are going to use ROM3 address
CMD_MAGIC_NUMBER    	  equ ($ABCD) 					  ; Magic number header to identify a command
CMD_RETRIES_COUNT	  	  equ 3							  ; Number of retries for the command
CMD_SET_SHARED_VAR		  equ 1							  ; This is a fake command to set the shared variables
														  ; Used to store the system settings

; MD-JS worker commands (must match js_worker.h CMD_JS_* values)
CMD_JS_PING    				equ $0010 ; Ping — detect worker, get version

_dskbufp                equ $4c6                            ; Address of the disk buffer pointer    


	include inc/sidecart_macros.s
	include inc/tos.s



; Macros
; XBIOS Get Screen Base
; Return the screen memory address in D0
get_screen_base		macro
					move.w #2,-(sp)
					trap #14
					addq.l #2,sp
					endm

	section

;Rom cartridge

	org ROM4_ADDR

	dc.l $abcdef42 					; magic number
first:
;	dc.l second
	dc.l 0
	dc.l $08000000 + pre_auto		; After GEMDOS init (before booting from disks)
	dc.l 0
	dc.w GEMDOS_TIME 				;time
	dc.w GEMDOS_DATE 				;date
	dc.l end_pre_auto - pre_auto
	dc.b "MDJS",0
    even

pre_auto:
; Relocate the content of the cartridge ROM to the RAM

; Get the screen memory address to display
	get_screen_base
	move.l d0, a2

	lea SCREEN_SIZE(a2), a2		; Move to the work area just after the screen memory
	move.l a2, a3				; Save the relocation destination address in A3
	; Copy the code out of the ROM to avoid unstable behavior
    move.l #end_rom_code - start_rom_code, d6
    lea start_rom_code, a1    ; a1 points to the start of the code in ROM
    lsr.w #2, d6
    subq #1, d6
.copy_rom_code:
    move.l (a1)+, (a2)+
    dbf d6, .copy_rom_code
	jmp (a3)

start_rom_code:
; Detect MD-JS worker (ping the RP2040 with CMD_JS_PING)
; D7 = 1 if worker is available, 0 otherwise (used by boot_gem path)
	send_sync CMD_JS_PING, 4	; payload = random token only (4 bytes)
	tst.w d0					; D0 = 0 on success, non-zero on timeout/error
	beq.s .js_found
	clr.l d7					; Worker not detected
	bra.s .js_detect_done
.js_found:
	move.l #1, d7				; Worker detected
.js_detect_done:
	bra boot_gem

boot_gem:
	; Print a startup message via GEMDOS Cconws, then return to GEM.
	; If the JS worker was detected (D7 = 1) show the ready message,
	; otherwise show a not-detected warning.
	tst.l d7
	beq.s .boot_gem_no_worker

	pea msg_ready
	move.w #9, -(sp)				; GEMDOS Cconws
	trap #1
	addq.l #6, sp
	bra.s .boot_gem_done

.boot_gem_no_worker:
	pea msg_not_detected
	move.w #9, -(sp)				; GEMDOS Cconws
	trap #1
	addq.l #6, sp

.boot_gem_done:
	rts

msg_ready:
	dc.b "MD/JS: JavaScript Worker is ready",$d,$a,0
	even

msg_not_detected:
	dc.b "MD/JS: JavaScript Worker not detected",$d,$a,0
	even

; Shared functions included at the end of the file
; Don't forget to include the macros for the shared functions at the top of file
    include "inc/sidecart_functions.s"


end_rom_code:
end_pre_auto:
	even
	dc.l 0
