/* Copyright 2007-2010 Jozsef Kadlecsik (kadlec@blackhole.kfki.hu)
 *
 * This program is free software; you can redistribute it and/or modify   
 * it under the terms of the GNU General Public License version 2 as 
 * published by the Free Software Foundation.
 */
#include <assert.h>				/* assert */
#include <arpa/inet.h>				/* ntoh* */
#include <net/ethernet.h>			/* ETH_ALEN */
#include <sys/socket.h>				/* AF_ */
#include <stdlib.h>				/* malloc, free */
#include <string.h>				/* memset */

#include <libipset/linux_ip_set.h>		/* IPSET_MAXNAMELEN */
#include <libipset/debug.h>			/* D() */
#include <libipset/types.h>			/* struct ipset_type */
#include <libipset/utils.h>			/* inXcpy */
#include <libipset/data.h>			/* prototypes */

/* Internal data structure to hold 
 * a) input data entered by the user or
 * b) data received from kernel
 *
 * We always store the data in host order, *except* IP addresses.
 */

struct ipset_data {
	/* Option bits: which fields are set */
	uint64_t bits;
	/* Option bits: which options are ignored */
	uint64_t ignored;
	/* Setname  */
	char setname[IPSET_MAXNAMELEN];
	/* Set type */
	const struct ipset_type *type;
	/* Common CADT options */
	uint8_t cidr;
	uint8_t family;
	uint32_t flags;		/* command level flags */
	uint32_t cadt_flags;	/* data level flags */
	uint32_t timeout;
	union nf_inet_addr ip;
	union nf_inet_addr ip_to;
	uint16_t port;
	uint16_t port_to;
	union {
		/* RENAME/SWAP */
		char setname2[IPSET_MAXNAMELEN];
		/* CREATE/LIST/SAVE */
		struct {
			uint8_t probes;
			uint8_t resize;
			uint8_t netmask;
			uint32_t hashsize;
			uint32_t maxelem;
			uint32_t gc;
			uint32_t size;
			/* Filled out by kernel */
			uint32_t references;
			uint32_t elements;
			uint32_t memsize;
			char typename[IPSET_MAXNAMELEN];
			uint8_t revision_min;
			uint8_t revision;
		} create;
		/* ADT/LIST/SAVE */
		struct {
			union nf_inet_addr ip2;
			uint8_t cidr2;
			uint8_t proto;
			char ether[ETH_ALEN];
			char name[IPSET_MAXNAMELEN];
			char nameref[IPSET_MAXNAMELEN];
		} adt;
	};
};

static void
copy_addr(uint8_t family, union nf_inet_addr *ip, const void *value)
{
	if (family == AF_INET)
		in4cpy(&ip->in, value);
	else
		in6cpy(&ip->in6, value);
}

/**
 * ipset_strlcpy - copy the string from src to dst
 * @dst: the target string buffer
 * @src: the source string buffer
 * @len: the length of bytes to copy, including the terminating null byte.
 *
 * Copy the string from src to destination, but at most len bytes are
 * copied. The target is unconditionally terminated by the null byte.
 */
void
ipset_strlcpy(char *dst, const char *src, size_t len)
{
	assert(dst);
	assert(src);

	strncpy(dst, src, len);
	dst[len - 1] = '\0';
}

/**
 * ipset_data_flags_test - test option bits in the data blob
 * @data: data blob
 * @flags: the option flags to test
 *
 * Returns true if the options are already set in the data blob.
 */
bool
ipset_data_flags_test(const struct ipset_data *data, uint64_t flags)
{
	assert(data);
	return !!(data->bits & flags);
}

/**
 * ipset_data_flags_set - set option bits in the data blob
 * @data: data blob
 * @flags: the option flags to set
 *
 * The function sets the flags in the data blob so that
 * the corresponding fields are regarded as if filled with proper data.
 */
void
ipset_data_flags_set(struct ipset_data *data, uint64_t flags)
{
	assert(data);
	data->bits |= flags;
}

/**
 * ipset_data_flags_unset - unset option bits in the data blob
 * @data: data blob
 * @flags: the option flags to unset
 *
 * The function unsets the flags in the data blob.
 * This is the quick way to clear specific fields.
 */
void
ipset_data_flags_unset(struct ipset_data *data, uint64_t flags)
{
	assert(data);
	data->bits &= ~flags;
}

#define flag_type_attr(data, opt, flag)		\
do {						\
	data->flags |= flag;			\
	opt = IPSET_OPT_FLAGS;			\
} while (0)

#define cadt_flag_type_attr(data, opt, flag)	\
do {						\
	data->cadt_flags |= flag;		\
	opt = IPSET_OPT_CADT_FLAGS;		\
} while (0)

/**
 * ipset_data_ignored - test and set ignored bits in the data blob
 * @data: data blob
 * @flags: the option flag to be ignored
 *
 * Returns true if the option was not already ignored.
 */
bool
ipset_data_ignored(struct ipset_data *data, enum ipset_opt opt)
{
	bool ignored;
	assert(data);
	
	ignored = data->ignored & IPSET_FLAG(opt);
	data->ignored |= IPSET_FLAG(opt);

	return ignored;
}

/**
 * ipset_data_set - put data into the data blob
 * @data: data blob
 * @opt: the option kind of the data
 * @value: the value of the data
 *
 * Put a given kind of data into the data blob and mark the
 * option kind as already set in the blob.
 *
 * Returns 0 on success or a negative error code.
 */
int
ipset_data_set(struct ipset_data *data, enum ipset_opt opt, const void *value)
{
	assert(data);
	assert(opt != IPSET_OPT_NONE);
	assert(value);

	switch (opt) {
	/* Common ones */
	case IPSET_SETNAME:
		ipset_strlcpy(data->setname, value, IPSET_MAXNAMELEN);
		break;
	case IPSET_OPT_TYPE:
		data->type = value;
		break;
	case IPSET_OPT_FAMILY:
		data->family = *(const uint8_t *) value;
		D("family set to %u", data->family);
		break;
	/* CADT options */
	case IPSET_OPT_IP:
		if (!(data->family == AF_INET || data->family == AF_INET6))
			return -1;
		copy_addr(data->family, &data->ip, value);
		break;
	case IPSET_OPT_IP_TO:
		if (!(data->family == AF_INET || data->family == AF_INET6))
			return -1;
		copy_addr(data->family, &data->ip_to, value);
		break;
	case IPSET_OPT_CIDR:
		data->cidr = *(const uint8_t *) value;
		break;
	case IPSET_OPT_PORT:
		data->port = *(const uint16_t *) value;
		break;
	case IPSET_OPT_PORT_TO:
		data->port_to = *(const uint16_t *) value;
		break;
	case IPSET_OPT_TIMEOUT:
		data->timeout = *(const uint32_t *) value;
		break;
	/* Create-specific options */
	case IPSET_OPT_GC:
		data->create.gc = *(const uint32_t *) value;
		break;
	case IPSET_OPT_HASHSIZE:
		data->create.hashsize = *(const uint32_t *) value;
		break;
	case IPSET_OPT_MAXELEM:
		data->create.maxelem = *(const uint32_t *) value;
		break;
	case IPSET_OPT_NETMASK:
		data->create.netmask = *(const uint8_t *) value;
		break;
	case IPSET_OPT_PROBES:
		data->create.probes = *(const uint8_t *) value;
		break;
	case IPSET_OPT_RESIZE:
		data->create.resize = *(const uint8_t *) value;
		break;
	case IPSET_OPT_SIZE:
		data->create.size = *(const uint32_t *) value;
		break;
	/* Create-specific options, filled out by the kernel */
	case IPSET_OPT_ELEMENTS:
		data->create.elements = *(const uint32_t *) value;
		break;
	case IPSET_OPT_REFERENCES:
		data->create.references = *(const uint32_t *) value;
		break;
	case IPSET_OPT_MEMSIZE:
		data->create.memsize = *(const uint32_t *) value;
		break;
	/* Create-specific options, type */
	case IPSET_OPT_TYPENAME:
		ipset_strlcpy(data->create.typename, value,
			      IPSET_MAXNAMELEN);
		break;
	case IPSET_OPT_REVISION:
		data->create.revision = *(const uint8_t *) value;
		break;
	case IPSET_OPT_REVISION_MIN:
		data->create.revision_min = *(const uint8_t *) value;
		break;
	/* ADT-specific options */
	case IPSET_OPT_ETHER:
		memcpy(data->adt.ether, value, ETH_ALEN);
		break;
	case IPSET_OPT_NAME:
		ipset_strlcpy(data->adt.name, value, IPSET_MAXNAMELEN);
		break;
	case IPSET_OPT_NAMEREF:
		ipset_strlcpy(data->adt.nameref, value, IPSET_MAXNAMELEN);
		break;
	case IPSET_OPT_IP2:
		if (!(data->family == AF_INET || data->family == AF_INET6))
			return -1;
		copy_addr(data->family, &data->adt.ip2, value);
		break;
	case IPSET_OPT_CIDR2:
		data->adt.cidr2 = *(const uint8_t *) value;
		break;
	case IPSET_OPT_PROTO:
		data->adt.proto = *(const uint8_t *) value;
		break;
	/* Swap/rename */
	case IPSET_OPT_SETNAME2:
		ipset_strlcpy(data->setname2, value, IPSET_MAXNAMELEN);
		break;
	/* flags */
	case IPSET_OPT_EXIST:
		flag_type_attr(data, opt, IPSET_FLAG_EXIST);
		break;
	case IPSET_OPT_BEFORE:
		cadt_flag_type_attr(data, opt, IPSET_FLAG_BEFORE);
		break;
	case IPSET_OPT_FLAGS:
		data->flags = *(const uint32_t *)value;
		break;
	case IPSET_OPT_CADT_FLAGS:
		data->cadt_flags = *(const uint32_t *)value;
		break;
	default:
		return -1;
	};
	
	ipset_data_flags_set(data, IPSET_FLAG(opt));
	return 0;
}

/**
 * ipset_data_get - get data from the data blob
 * @data: data blob
 * @opt: option kind of the requested data
 *
 * Returns the pointer to the requested kind of data from the data blob
 * if it is set. If the option kind is not set or is an unkown type,
 * NULL is returned.
 */
const void *
ipset_data_get(const struct ipset_data *data, enum ipset_opt opt)
{
	assert(data);
	assert(opt != IPSET_OPT_NONE);
	
	if (!(opt == IPSET_OPT_TYPENAME || ipset_data_test(data, opt)))
		return NULL;

	switch (opt) {
	/* Common ones */
	case IPSET_SETNAME:
		return data->setname;
	case IPSET_OPT_TYPE:
		return data->type;
	case IPSET_OPT_TYPENAME:
		if (ipset_data_test(data, IPSET_OPT_TYPE))
			return data->type->name;
		else if (ipset_data_test(data, IPSET_OPT_TYPENAME))
			return data->create.typename;
		return NULL;
	case IPSET_OPT_FAMILY:
		return &data->family;
	/* CADT options */
	case IPSET_OPT_IP:
		return &data->ip;
	case IPSET_OPT_IP_TO:
		 return &data->ip_to;
	case IPSET_OPT_CIDR:
		return &data->cidr;
	case IPSET_OPT_PORT:
		return &data->port;
	case IPSET_OPT_PORT_TO:
		return &data->port_to;
	case IPSET_OPT_TIMEOUT:
		return &data->timeout;
	/* Create-specific options */
	case IPSET_OPT_GC:
		return &data->create.gc;
	case IPSET_OPT_HASHSIZE:
		return &data->create.hashsize;
	case IPSET_OPT_MAXELEM:
		return &data->create.maxelem;
	case IPSET_OPT_NETMASK:
		return &data->create.netmask;
	case IPSET_OPT_PROBES:
		return &data->create.probes;
	case IPSET_OPT_RESIZE:
		return &data->create.resize;
	case IPSET_OPT_SIZE:
		return &data->create.size;
	/* Create-specific options, filled out by the kernel */
	case IPSET_OPT_ELEMENTS:
		return &data->create.elements;
	case IPSET_OPT_REFERENCES:
		return &data->create.references;
	case IPSET_OPT_MEMSIZE:
		return &data->create.memsize;
	/* Create-specific options, TYPE */
	case IPSET_OPT_REVISION:
		return &data->create.revision;
	case IPSET_OPT_REVISION_MIN:
		return &data->create.revision_min;
	/* ADT-specific options */
	case IPSET_OPT_ETHER:
		return data->adt.ether;
	case IPSET_OPT_NAME:
		return data->adt.name;
	case IPSET_OPT_NAMEREF:
		return data->adt.nameref;
	case IPSET_OPT_IP2:
		return &data->adt.ip2;
	case IPSET_OPT_CIDR2:
		return &data->adt.cidr2;
	case IPSET_OPT_PROTO:
		return &data->adt.proto;
	/* Swap/rename */
	case IPSET_OPT_SETNAME2:
		return data->setname2;
	/* flags */
	case IPSET_OPT_FLAGS:
	case IPSET_OPT_EXIST:
		return &data->flags;
	case IPSET_OPT_CADT_FLAGS:
	case IPSET_OPT_BEFORE:
		return &data->cadt_flags;
	default:
		return NULL;
	}
}

/**
 * ipset_data_sizeof - calculates the size of the data type
 * @opt: option kind of the data
 * @family: INET family
 *
 * Returns the size required to store the given data type.
 */
size_t
ipset_data_sizeof(enum ipset_opt opt, uint8_t family)
{
	assert(opt != IPSET_OPT_NONE);

	switch (opt) {
	case IPSET_OPT_IP:
	case IPSET_OPT_IP_TO:
	case IPSET_OPT_IP2:
		return family == AF_INET ? sizeof(uint32_t)
					 : sizeof(struct in6_addr);
	case IPSET_OPT_PORT:
	case IPSET_OPT_PORT_TO:
		return sizeof(uint16_t);
	case IPSET_SETNAME:
	case IPSET_OPT_NAME:
	case IPSET_OPT_NAMEREF:
		return IPSET_MAXNAMELEN;
	case IPSET_OPT_TIMEOUT:
	case IPSET_OPT_GC:
	case IPSET_OPT_HASHSIZE:
	case IPSET_OPT_MAXELEM:
	case IPSET_OPT_SIZE:
	case IPSET_OPT_ELEMENTS:
	case IPSET_OPT_REFERENCES:
	case IPSET_OPT_MEMSIZE:
		return sizeof(uint32_t);
	case IPSET_OPT_CIDR:
	case IPSET_OPT_CIDR2:
	case IPSET_OPT_NETMASK:
	case IPSET_OPT_PROBES:
	case IPSET_OPT_RESIZE:
	case IPSET_OPT_PROTO:
		return sizeof(uint8_t);
	case IPSET_OPT_ETHER:
		return ETH_ALEN;
	/* Flags counted once */
	case IPSET_OPT_BEFORE:
		return sizeof(uint32_t);
	default:
		return 0;
	};
}

/**
 * ipset_setname - return the name of the set from the data blob
 * @data: data blob
 *
 * Return the name of the set from the data blob or NULL if the
 * name not set yet.
 */
const char *
ipset_data_setname(const struct ipset_data *data)
{
	assert(data);
	return ipset_data_test(data, IPSET_SETNAME) ? data->setname : NULL;
}

/**
 * ipset_family - return the INET family of the set from the data blob
 * @data: data blob
 *
 * Return the INET family supported by the set from the data blob.
 * If the family is not set yet, AF_UNSPEC is returned.
 */
uint8_t
ipset_data_family(const struct ipset_data *data)
{
	assert(data);
	return ipset_data_test(data, IPSET_OPT_FAMILY)
		? data->family : AF_UNSPEC;
}

/**
 * ipset_data_cidr - return the value of IPSET_OPT_CIDR
 * @data: data blob
 *
 * Return the value of IPSET_OPT_CIDR stored in the data blob.
 * If it is not set, then the returned value corresponds to
 * the default one according to the family type or zero.
 */
uint8_t
ipset_data_cidr(const struct ipset_data *data)
{
	assert(data);
	return ipset_data_test(data, IPSET_OPT_CIDR) ? data->cidr : 
	       data->family == AF_INET ? 32 : 
	       data->family == AF_INET6 ? 128 : 0;
}

/**
 * ipset_flags - return which fields are set in the data blob
 * @data: data blob
 *
 * Returns the value of the bit field which elements are set.
 */
uint64_t
ipset_data_flags(const struct ipset_data *data)
{
	assert(data);
	return data->bits;
}

/**
 * ipset_data_reset - reset the data blob to unset
 * @data: data blob
 *
 * Resets the data blob to the unset state for every field.
 */
void
ipset_data_reset(struct ipset_data *data)
{
	assert(data);
	memset(data, 0, sizeof(*data));
}

/**
 * ipset_data_init - create a new data blob
 *
 * Return the new data blob initialized to empty. In case of
 * an error, NULL is retured.
 */
struct ipset_data *
ipset_data_init(void)
{
	return calloc(1, sizeof(struct ipset_data));
}

/**
 * ipset_data_fini - release a data blob created by ipset_data_init
 *
 * Release the data blob created by ipset_data_init previously.
 */
void
ipset_data_fini(struct ipset_data *data)
{
	assert(data);
	free(data);
}
