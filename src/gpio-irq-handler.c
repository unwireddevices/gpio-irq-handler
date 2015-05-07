/*
 *  GPIO IRQ handler for AR9331
 *
 *    Copyright (C) 2013-2015 Gerhard Bertelsmann <info@gerhard-bertelsmann.de>
 *    Copyright (C) 2015 Dmitriy Zherebkov <dzh@black-swift.com>
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License version 2 as published
 *  by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>

#include <asm/mach-ath79/ar71xx_regs.h>
#include <asm/mach-ath79/ath79.h>
#include <asm/mach-ath79/irq.h>

#include <asm/siginfo.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>

//#define DEBUG_OUT

#ifdef	DEBUG_OUT
#define debug(fmt,args...)	printk (KERN_INFO fmt ,##args)
#else
#define debug(fmt,args...)
#endif	/* DEBUG_OUT */

//#define SIG_GPIO_IRQ	(SIGRTMIN+10)	// SIGRTMIN is different in Kernel and User modes
#define SIG_GPIO_IRQ	42				// So we have to hardcode this value

////////////////////////////////////////////////////////////////////////////////////////////

void __iomem *ath79_gpio_base;

#define DRV_NAME	"GPIO IRQ handler"
#define FILE_NAME	"gpio-irq"

////////////////////////////////////////////////////////////////////////////////////////////

#define MAX_PROCESSES	10

typedef struct
{
	int		gpio;
	int		irq;
	int		last_value;

	pid_t	processes[MAX_PROCESSES];
} _gpio_handler;

#define TOTAL_GPIO	30

static _gpio_handler	all_handlers[TOTAL_GPIO];

static struct dentry* in_file;

////////////////////////////////////////////////////////////////////////////////////////////

static int is_space(char symbol)
{
	return (symbol == ' ') || (symbol == '\t');
}

////////////////////////////////////////////////////////////////////////////////////////////

static int is_digit(char symbol)
{
	return (symbol >= '0') && (symbol <= '9');
}

////////////////////////////////////////////////////////////////////////////////////////////

static int is_eol(char symbol)
{
	return (symbol == '\n') || (symbol == '\r');
}

////////////////////////////////////////////////////////////////////////////////////////////

static irqreturn_t gpio_edge_interrupt(int irq, void* dev_id)
{
	_gpio_handler* handler=(_gpio_handler*)dev_id;

	if(handler && (handler->irq == irq))
	{
		int val=0;

		debug("Got _handler!\n");

		val=(__raw_readl(ath79_gpio_base + AR71XX_GPIO_REG_IN) >> handler->gpio) & 1;

		if(val != handler->last_value)
		{
			struct siginfo info;
			struct task_struct* ts=NULL;

			int i=0;

			handler->last_value=val;
			debug("IRQ %d event (GPIO %d) - new value is %d!\n", irq, handler->gpio, val);

			/* send the signal */
			memset(&info, 0, sizeof(struct siginfo));
			info.si_signo = SIG_GPIO_IRQ;
			info.si_code = SI_QUEUE;	// this is bit of a trickery: SI_QUEUE is normally used by sigqueue from user space,
							// and kernel space should use SI_KERNEL. But if SI_KERNEL is used the real_time data
							// is not delivered to the user space signal handler function.

			for(i=0; i < MAX_PROCESSES; ++i)
			{
				pid_t pid=handler->processes[i];

				if(pid == 0) break;

				info.si_int=(handler->gpio << 24) | (val & 1);

				rcu_read_lock();
				ts=pid_task(find_vpid(pid), PIDTYPE_PID);
				rcu_read_unlock();

				if(ts == NULL)
				{
					debug("PID %u is not found, remove it please.\n",pid);
				}
				else
				{
					debug("Sending signal to PID %u.\n",pid);
					send_sig_info(SIG_GPIO_IRQ, &info, ts);    //send the signal
				}
			}
		}
	}
	else
	{
		debug("IRQ %d event - no handlers found!\n",irq);
	}

	return(IRQ_HANDLED);
}

////////////////////////////////////////////////////////////////////////////////////////////

static int add_irq(int gpio,void* data)
{
    if(gpio_request(gpio, DRV_NAME) >= 0)
    {
		int irq_number=gpio_to_irq(gpio);

		if(irq_number >= 0)
		{
		    int err = request_irq(irq_number, gpio_edge_interrupt, IRQ_TYPE_EDGE_BOTH, "gpio_irq_handler", data);

		    if(!err)
		    {
		    	debug("Got IRQ %d for GPIO %d\n", irq_number, gpio);
				return irq_number;
		    }
		    else
		    {
		    	debug("GPIO IRQ handler: trouble requesting IRQ %d error %d\n",irq_number, err);
		    }
		}
		else
		{
			debug("Can't map GPIO %d to IRQ : error %d\n",gpio, irq_number);
		}
    }
    else
    {
    	debug("Can't get GPIO %d\n", gpio);
    }

    return -1;
}

////////////////////////////////////////////////////////////////////////////////////////////

static void free_handler(int gpio)
{
	if((gpio >= 0) && (gpio < TOTAL_GPIO))
	{
		_gpio_handler* handler=&all_handlers[gpio];

		if(handler->gpio == gpio)
		{
			int i=0;

		    if(handler->irq >= 0)
			{
				free_irq(handler->irq, (void*)handler);
				handler->irq=-1;
			}

		    gpio_free(gpio);
			handler->gpio=-1;

			for(; i < MAX_PROCESSES; ++i)
			{
				handler->processes[i]=0;
			}
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////

static void remove_handler(int gpio,pid_t pid)
{
	if((gpio >= 0) && (gpio < TOTAL_GPIO))
	{
		_gpio_handler* handler=&all_handlers[gpio];

		if(handler->gpio == gpio)
		{
			int i=0;

			for(i=0; i < MAX_PROCESSES; ++i)
			{
				if(handler->processes[i] == pid)
				{
					for(++i; i < MAX_PROCESSES; ++i)
					{
						handler->processes[i-1]=handler->processes[i];
					}
					handler->processes[i-1]=0;

					return;
				}
			}

			debug("Handler for GPIO %d to PID %u is not found.\n", gpio, pid);
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////

static int add_handler(int gpio, pid_t pid)
{
	if((gpio >= 0) && (gpio < TOTAL_GPIO))
	{
		_gpio_handler* handler=&all_handlers[gpio];
		int p=0;

		if(handler->gpio != gpio)
		{
			int irq=add_irq(gpio, handler);

			if(irq < 0)
			{
				free_handler(gpio);
				return -1;
			}

			handler->gpio=gpio;
			handler->irq=irq;
			handler->last_value=-1;
		}

		while((handler->processes[p] > 0) && (handler->processes[p] != pid)) ++p;

		if(p < MAX_PROCESSES)
		{
			handler->processes[p]=pid;
			return handler->irq;
		}
		else
		{
			debug("Can't add handler: %d processes already handle GPIO %d.\n", p, gpio);
		}
	}

	return -1;
}

////////////////////////////////////////////////////////////////////////////////////////////

static ssize_t run_command(struct file *file, const char __user *buf,
                                size_t count, loff_t *ppos)
{
	char buffer[512];
	char line[20];
	char* in_pos=NULL;
	char* end=NULL;
	char* out_pos=NULL;

	int add=0;
	int gpio=-1;
	int pid=0;

	if(count > 512)
		return -EINVAL;	//	file is too big

	copy_from_user(buffer, buf, count);
	buffer[count]=0;

	debug("Command is found (%u bytes length):\n%s\n",count,buffer);

	in_pos=buffer;
	end=in_pos+count-1;

	while(in_pos < end)
	{
		add=0;
		gpio=-1;
		pid=0;

		while((in_pos < end) && is_space(*in_pos)) ++in_pos;	//	skip whitespace
		if(in_pos >= end) break;

		if(*in_pos == '+')
		{
			add=1;
		}
		else if(*in_pos == '-')
		{
			add=0;
		}
		else if(*in_pos == '?')
		{
			//	just print all handlers
			int i,j;

			for(i=0; i < TOTAL_GPIO; ++i)
			{
				if(all_handlers[i].gpio != -1)
				{
					printk(KERN_INFO "GPIO %d (IRQ %d): ",all_handlers[i].gpio,all_handlers[i].irq);

					for(j=0; j < MAX_PROCESSES; ++j)
					{
						if(all_handlers[i].processes[j] != 0)
						{
							printk("%u ",all_handlers[i].processes[j]);
						}
						else
						{
							break;
						}
					}

					printk("\n");
				}
			}

			return count;
		}
		else
		{
			printk(KERN_INFO "Wrong command '%c'.\n", *in_pos);
			break;
		}
		++in_pos;
		while((in_pos < end) && is_space(*in_pos)) ++in_pos;	//	skip whitespace

		out_pos=line;
		while((in_pos < end) && is_digit(*in_pos)) *out_pos++=*in_pos++;
		*out_pos=0;

		if(is_digit(line[0]))
		{
			sscanf(line, "%d", &gpio);
		}
		else
		{
			printk(KERN_INFO "Can't read GPIO number.\n");
			break;
		}

		while((in_pos < end) && is_space(*in_pos)) ++in_pos;	//	skip whitespace

		out_pos=line;
		while((in_pos < end) && is_digit(*in_pos)) *out_pos++=*in_pos++;
		*out_pos=0;

		if(is_digit(line[0]))
		{
			sscanf(line, "%u", &pid);
		}
		else
		{
			if(add)
			{
				printk(KERN_INFO "Can't read PID.\n");
				break;
			}
		}

		if(add)
		{
			debug("Trying to add handler for GPIO %d to PID %u.\n",gpio,pid);
			add_handler(gpio,pid);
		}
		else
		{
			if(pid)
			{
				_gpio_handler* handler=&all_handlers[gpio];

				debug("Trying to remove handler for GPIO %d to PID %u.\n",gpio,pid);
				remove_handler(gpio,pid);

				if(handler->processes[0] == 0)
				{
					debug("It was the last handler. Let's free IRQ %d.\n",handler->irq);
					free_handler(gpio);
				}
			}
			else
			{
				debug("Trying to remove all handlers for GPIO %d.\n",gpio);
				free_handler(gpio);
			}
		}

		while((in_pos < end) && (is_space(*in_pos) || is_eol(*in_pos))) ++in_pos;	// next line
	}

	return count;
}

////////////////////////////////////////////////////////////////////////////////////////////

static const struct file_operations irq_fops = {
//	.read = show_handlers,
	.write = run_command,
};

////////////////////////////////////////////////////////////////////////////////////////////

static int __init mymodule_init(void)
{
    int i=0,j=0;

	ath79_gpio_base = ioremap_nocache(AR71XX_GPIO_BASE, AR71XX_GPIO_SIZE);

	for(i=0; i < TOTAL_GPIO; ++i)
	{
		all_handlers[i].gpio=-1;
		all_handlers[i].irq=-1;
		all_handlers[i].gpio=-1;

		for(j=0; j < MAX_PROCESSES; ++j)
		{
			all_handlers[i].processes[j]=0;
		}
	}

	in_file=debugfs_create_file(FILE_NAME, 0666, NULL, NULL, &irq_fops);

	debug("Waiting for commands in file /sys/kernel/debug/" FILE_NAME ".\n");

    return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////

static void __exit mymodule_exit(void)
{
	int i=0;

	for(; i < TOTAL_GPIO; ++i)
	{
		free_handler(i);
	}

	debugfs_remove(in_file);

	return;
}

////////////////////////////////////////////////////////////////////////////////////////////

module_init(mymodule_init);
module_exit(mymodule_exit);

////////////////////////////////////////////////////////////////////////////////////////////

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dmitriy Zherebkov (Black Swift team)");

////////////////////////////////////////////////////////////////////////////////////////////
