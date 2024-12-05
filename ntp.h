#ifdef __cplusplus
extern "C" {
#endif
typedef struct NTP_T_ NTP_T;

NTP_T* ntp_init(void);

void ntp_update(NTP_T *state);

#ifdef __cplusplus
}
#endif