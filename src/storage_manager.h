#include <zephyr.h>
#include <sys/reboot.h>
#include <device.h>
#include <string.h>
#include <drivers/flash.h>
#include <storage/flash_map.h>
#include <fs/nvs.h>

#define INIT_ERROR -1

#define URL_SIZE 48
#define USERNAME_SIZE 24
#define PWD_SIZE 24

#define MAX_STORABLE_PWD 24

typedef struct TPassword{
	char url[URL_SIZE];
	char username[USERNAME_SIZE];
	char pwd[PWD_SIZE];
};

/**
 * @brief Initialize store manager
*/
int store_manager_init();

/**
 * @brief Get the stored password for certain URL and username
 * 
 * @param pwdStruct Struct containing the URL and username, in which the password will be entered
*/
int getPwd(struct TPassword *pwdStruct);

/**
 * @brief Get all the stored password. Returns number of password stored
 * 
 * @param pwdList Pointer to array list of TPassword. Array size must be at least MAX_STORABLE_PWD
*/
int getAllPwd(struct TPassword *pwdList);

/**
 * @brief Store the given password assigned to the given URL and username
 * 
 * @param pwdStruct Struct containing URL, username and password to be stored
*/
int storePwd(const struct TPassword *pwdStruct);

/**
 * @brief Delete all stored passwords
*/
void deleteAllPwd();