// SPDX-License-Identifier: GPL-2.0+
/*
 * Retu/Tahvo direct register access driver
 *
 * Copyright (C) 2021 Peter Vasil <petervasil@gmail.com>
 *
 */

#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/mfd/retu.h>

struct retu_regs {
	struct kobject kobj;
	struct device *dev;
	struct retu_dev *rdev;
	int nregs;
};

static ssize_t retu_regs_name_show(struct retu_regs *rregs, u8 reg, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", rregs->dev->of_node->name);
}

static ssize_t retu_regs_nregs_show(struct retu_regs *rregs, u8 reg, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", rregs->nregs);
}

static ssize_t retu_regs_dump_show(struct retu_regs *rregs, u8 reg, char *buf)
{
	int ccnt;
	u8 r;
	ccnt = 0;
	for (r=0; r<rregs->nregs; r++) {
		u16 v = retu_read(rregs->rdev, r);
		if ((r&0x07) == 0) {
			if (r > 0)
				ccnt += snprintf(buf+ccnt, PAGE_SIZE-ccnt, "\n");
			ccnt += snprintf(buf+ccnt, PAGE_SIZE-ccnt, "%02x : ", r);
		}
		if ((r&0x07) == 4)
			ccnt += snprintf(buf+ccnt, PAGE_SIZE-ccnt, " ");
		ccnt += snprintf(buf+ccnt, PAGE_SIZE-ccnt, " %04x", v);
	}
	ccnt += snprintf(buf+ccnt, PAGE_SIZE-ccnt, "\n");
	return ccnt;
}

static ssize_t retu_regs_single_show(struct retu_regs *rregs, u8 reg, char *buf)
{
	u16 v;

	if (reg >= rregs->nregs)
		return -ENOENT;

	v = retu_read(rregs->rdev, reg);
	buf[0] = v & 0xff;
	buf[1] = (v>>8) & 0xff;

	return 2;
}

static ssize_t retu_regs_single_store(struct retu_regs *rregs, u8 reg, const char *buf, size_t size)
{
	u16 v;

	if (size != 2)
		return -EINVAL;
	if (reg >= rregs->nregs)
		return -ENOENT;

	v = (buf[1] << 8) | buf[0];
	retu_write(rregs->rdev, reg, v);

	return size;
}

struct retu_regs_attribute {
	struct attribute attr;
	ssize_t (*show)(struct retu_regs *, u8, char *);
	ssize_t (*store)(struct retu_regs *, u8, const char *, size_t);
	u8 reg;
};

#define RETU_REGS_ATTR(_name, _mode, _show, _store) \
	struct retu_regs_attribute retu_regs_attr_##_name = \
	__ATTR(_name, _mode, _show, _store)

static RETU_REGS_ATTR(name, S_IRUGO, retu_regs_name_show, NULL);
static RETU_REGS_ATTR(nregs, S_IRUGO, retu_regs_nregs_show, NULL);
static RETU_REGS_ATTR(dump, S_IRUGO, retu_regs_dump_show, NULL);

static struct attribute *retu_regs_sysfs_attrs[] = {
	&retu_regs_attr_name.attr,
	&retu_regs_attr_nregs.attr,
	&retu_regs_attr_dump.attr,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL
};
#define ATTR_BASE 3

static ssize_t retu_regs_attr_show(struct kobject *kobj, struct attribute *attr,
		char *buf)
{
	struct retu_regs *rregs;
	struct retu_regs_attribute *rregs_attr;

	rregs = container_of(kobj, struct retu_regs, kobj);
	rregs_attr = container_of(attr, struct retu_regs_attribute, attr);

	if (!rregs_attr->show)
		return -ENOENT;

	return rregs_attr->show(rregs, rregs_attr->reg, buf);
}

static ssize_t retu_regs_attr_store(struct kobject *kobj, struct attribute *attr,
		const char *buf, size_t size)
{
	struct retu_regs *rregs;
	struct retu_regs_attribute *rregs_attr;

	rregs = container_of(kobj, struct retu_regs, kobj);
	rregs_attr = container_of(attr, struct retu_regs_attribute, attr);

	if (!rregs_attr->store)
		return -ENOENT;

	return rregs_attr->store(rregs, rregs_attr->reg, buf, size);
}

static const struct sysfs_ops retu_regs_sysfs_ops = {
	.show = retu_regs_attr_show,
	.store = retu_regs_attr_store,
};

static struct kobj_type retu_regs_ktype = {
	.sysfs_ops = &retu_regs_sysfs_ops,
	.default_attrs = retu_regs_sysfs_attrs,
};

static int retu_regs_probe(struct platform_device *pdev)
{
	struct retu_dev *rdev = dev_get_drvdata(pdev->dev.parent);
	struct retu_regs *rregs;
	u8 r;

	if (!pdev->dev.of_node) {
		dev_err(&pdev->dev, "No OF node found!\n");
		return -EINVAL;
	}

	if (!pdev->dev.of_node->name) {
		dev_err(&pdev->dev, "No name found in OF node!\n");
		return -EINVAL;
	}

	rregs = devm_kzalloc(&pdev->dev, sizeof(*rregs), GFP_KERNEL);
	if (!rregs)
		return -ENOMEM;

	rregs->dev = &pdev->dev;
	rregs->rdev = rdev;
	of_property_read_u32(pdev->dev.of_node, "nregs", &rregs->nregs);

	dev_set_drvdata(&pdev->dev, rregs);

	for (r = 0; r < ATTR_BASE; r++) {
		struct retu_regs_attribute *rra = (struct retu_regs_attribute *)retu_regs_sysfs_attrs[r];
		rra->reg = 0xff;
	}
	for (r = ATTR_BASE; r < sizeof(retu_regs_sysfs_attrs)/sizeof(retu_regs_sysfs_attrs[0]); r++)
		retu_regs_sysfs_attrs[r] = NULL;

	for (r = 0; r < rregs->nregs; r++) {
		struct retu_regs_attribute *rra = devm_kzalloc(&pdev->dev, sizeof(struct retu_regs_attribute), GFP_KERNEL);
		char *name = devm_kzalloc(&pdev->dev, 3, GFP_KERNEL);
		sprintf(name, "%02x", r);
		rra->attr.name = name;
		rra->attr.mode = S_IRUGO|S_IWUSR;
		rra->show = retu_regs_single_show;
		rra->store = retu_regs_single_store;
		rra->reg = r;
		retu_regs_sysfs_attrs[ATTR_BASE+r] = &rra->attr;
	}

	return kobject_init_and_add(&rregs->kobj, &retu_regs_ktype,
		&pdev->dev.kobj, pdev->dev.of_node->name);
}

static int retu_regs_remove(struct platform_device *pdev)
{
	struct retu_regs *rregs = dev_get_drvdata(&pdev->dev);
	kobject_del(&rregs->kobj);
	kobject_put(&rregs->kobj);
	return 0;
}

static const struct of_device_id of_retu_regs_match[] = {
	{ .compatible = "nokia,retu,regs", },
	{},
};

static struct platform_driver retu_regs_driver = {
	.probe		= retu_regs_probe,
	.remove		= retu_regs_remove,
	.driver		= {
		.name	= "retu-regs",
		.of_match_table = of_match_ptr(of_retu_regs_match),
	},
};
module_platform_driver(retu_regs_driver);

MODULE_ALIAS("platform:retu-regs");
MODULE_DESCRIPTION("Retu/Tahvo register access");
MODULE_AUTHOR("Peter Vasil <petervasil@gmail.com>");
MODULE_LICENSE("GPL");
