#ifndef _ASM_MCE_H
#define _ASM_MCE_H 1

#include <asm/ioctls.h>
#include <asm/types.h>

/* 
 * Machine Check support for x86
 */
//IA32_MCG_CAP������Ϣ�Ĵ���
#define MCG_CTL_P        (1UL<<8)   /* MCG_CAP register available ������ǰ������ʵ����IA32_MCG_CTL*/
//IA32_MCG_STAUTS״̬�Ĵ����е�״̬λ
#define MCG_STATUS_RIPV  (1UL<<0)   /* restart ip valid ��ʾ����MC���Կ��ŵķ�ʽ����װ��EIPָ��*/
#define MCG_STATUS_EIPV  (1UL<<1)   /* eip points to correct instruction ��ʾEIPΪ���������ָ��*/
#define MCG_STATUS_MCIP  (1UL<<2)   /* machine check in progress ��ʾ������һ��MC*/
//bank�Ĵ������е�״̬�Ĵ���
#define MCI_STATUS_VAL   (1UL<<63)  /* valid error ��ζ�Ŵ�״̬�Ĵ����еĴ�����Ϣ��Ч*/
#define MCI_STATUS_OVER  (1UL<<62)  /* previous errors lost ��������MCǶ�ף�֮ǰ�Ĵ���ᶪʧ*/
#define MCI_STATUS_UC    (1UL<<61)  /* uncorrected error ��ʾ���ܿ�Ӳ��������*/
#define MCI_STATUS_EN    (1UL<<60)  /* error enabled ������IA32_MCi_CTL�ж�Ӧ��EFjλ������*/
#define MCI_STATUS_MISCV (1UL<<59)  /* misc error reg. valid ���� IA32_MCi_MISC�Ĵ����а����˴���Ķ�����Ϣ*/
#define MCI_STATUS_ADDRV (1UL<<58)  /* addr reg. valid �ʹ��� IA32_MCi_ADDR�Ĵ����к��з�������ʱ����ڴ��ַ*/
#define MCI_STATUS_PCC   (1UL<<57)  /* processor context corrupt��ζ����������������̽�⵽�Ĵ�����Ⱦ�ˣ�û�������޸���������ִ��ָ��*/

/* Fields are zero when not available */
struct mce {
	__u64 status;	//bank���е�״̬�Ĵ���ֵ
	__u64 misc;		//����Ĵ�����Ϣ
	__u64 addr;		//��������ʱ���ڴ��ַ
	__u64 mcgstatus; //ȫ��IA32_MCG_STAUTS��״̬�Ĵ���ֵ
	__u64 rip;		//ָ��ָ��Ĵ���
	__u64 tsc;	/* cpu time stamp counter ʱ�����ʱ��*/
	__u64 res1;	/* for future extension */	
	__u64 res2;	/* dito. */
	__u8  cs;		/* code segment */
	__u8  bank;	/* machine check bank��bank������*/
	__u8  cpu;	/* cpu that raised the error */
	__u8  finished;   /* entry is valid */
	__u32 pad;   
};

/* 
 * This structure contains all data related to the MCE log.
 * Also carries a signature to make it easier to find from external debugging tools.
 * Each entry is only valid when its finished flag is set.
 */

#define MCE_LOG_LEN 32

struct mce_log { 
	char signature[12]; /* "MACHINECHECK" */ 
	unsigned len;  	    /* = MCE_LOG_LEN */ 
	unsigned next;
	unsigned flags;
	unsigned pad0; 
	struct mce entry[MCE_LOG_LEN];
};

#define MCE_OVERFLOW 0		/* bit 0 in flags means overflow */

#define MCE_LOG_SIGNATURE 	"MACHINECHECK"

#define MCE_GET_RECORD_LEN   _IOR('M', 1, int)
#define MCE_GET_LOG_LEN      _IOR('M', 2, int)
#define MCE_GETCLEAR_FLAGS   _IOR('M', 3, int)

/* Software defined banks */
#define MCE_EXTENDED_BANK	128
#define MCE_THERMAL_BANK	MCE_EXTENDED_BANK + 0

void mce_log(struct mce *m);
#ifdef CONFIG_X86_MCE_INTEL
void mce_intel_feature_init(struct cpuinfo_x86 *c);
#else
static inline void mce_intel_feature_init(struct cpuinfo_x86 *c)
{
}
#endif

#endif
