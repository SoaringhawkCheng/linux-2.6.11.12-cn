#ifndef __ASM_HARDIRQ_H
#define __ASM_HARDIRQ_H

#include <linux/config.h>
#include <linux/threads.h>
#include <linux/irq.h>

typedef struct {  //irq_stat[NR_CPUS]����
	/**
	 * ÿ��CPU�Ϲ�������жϡ�
	 */
	unsigned int __softirq_pending; //ÿһλ��Ӧһ�����ж�
	unsigned long idle_timestamp; //CPU����ʱ��
	unsigned int __nmi_count;	/* arch dependent ��NMI�жϷ����Ĵ���*/
	unsigned int apic_timer_irqs;	/* arch dependent ������APICʱ���жϷ����Ĵ���*/
} ____cacheline_aligned irq_cpustat_t;

#include <linux/irq_cpustat.h>	/* Standard mappings for irq_cpustat_t above */

void ack_bad_irq(unsigned int irq);

#endif /* __ASM_HARDIRQ_H */
