#ifndef SEND_SCHEDULER_H_
#define SEND_SCHEDULER_H_

#include <stdbool.h>
#include <zephyr/net/lwm2m.h>

int send_scheduler_init(void);
bool send_scheduler_cache_filter(const struct lwm2m_obj_path *path,
				 const struct lwm2m_time_series_elem *element);

#endif /* SEND_SCHEDULER_H_ */
