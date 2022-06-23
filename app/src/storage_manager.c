#include "storage_manager.h"

#include "errno.h"

#define STORAGE_NODE_LABEL storage

#define NUM_PWD_ID 1
#define PWD_LIST_ID 2

static struct nvs_fs fs;
const struct device *flash_dev;
struct flash_pages_info info;

uint32_t numPwd;

int store_manager_init(){
    int rc = 0;
    /* define the nvs file system by settings with:
	 *	sector_size equal to the pagesize,
	 *	3 sectors
	 *	starting at FLASH_AREA_OFFSET(storage)
	 */
	flash_dev = FLASH_AREA_DEVICE(STORAGE_NODE_LABEL);
	if (!device_is_ready(flash_dev)) {
		printk("Flash device %s is not ready\n", flash_dev->name);
		return INIT_ERROR;
	}
	fs.offset = FLASH_AREA_OFFSET(storage);
	rc = flash_get_page_info_by_offs(flash_dev, fs.offset, &info);
	if (rc) {
		printk("Unable to get page info\n");
		return INIT_ERROR;
	}
	fs.sector_size = info.size;
	fs.sector_count = 3U;

	rc = nvs_init(&fs, flash_dev->name);
	if (rc) {
		printk("Flash Init failed\n");
		return INIT_ERROR;
	}

    /* Get how many password are stored now */
    rc = nvs_read(&fs, NUM_PWD_ID, &numPwd, sizeof(numPwd));
    if(rc <= 0){
        /* Item was not found */
		numPwd = 0;
        (void)nvs_write(&fs, NUM_PWD_ID, &numPwd, sizeof(numPwd));
    }

    return 0;
}

int getPwd(struct TPassword *pwdStruct){
    int rc = 0;
    struct TPassword pwdList[MAX_STORABLE_PWD];

    rc = nvs_read(&fs, PWD_LIST_ID, &pwdList, sizeof(pwdList));
    if(rc > 0){
        /* List was found */
        int i = 0;
        while(i < numPwd){
            if(strcmp(pwdStruct->url, pwdList[i].url) == 0 && strcmp(pwdStruct->username, pwdList[i].username) == 0){
                strcpy(pwdStruct->pwd, pwdList[i].pwd);
                return 0;
            }else{
                i++;
            }
        }
        /* Password not found */
        rc = -1;
    }else{
        strcpy(pwdStruct->pwd, "errno");
    }
    return rc;
}

int getAllPwd(struct TPassword *pwdList){
    int rc = 0;

    struct TPassword pwdListAux[MAX_STORABLE_PWD];

    if(numPwd > 0){
        rc = nvs_read(&fs, PWD_LIST_ID, pwdListAux, sizeof(pwdListAux));
        if(rc > 0){
            for(int i = 0; i < numPwd; i++){
                strcpy(pwdList[i].url, pwdListAux[i].url);
                strcpy(pwdList[i].username, pwdListAux[i].username);
                strcpy(pwdList[i].pwd, pwdListAux[i].pwd);
            }
            return numPwd;
        }else{
            return rc;
        }
    }
    return 0;
}

int storePwd(const struct TPassword *pwdStruct){
    int rc = 0;
    struct TPassword pwdList[MAX_STORABLE_PWD];

    rc = nvs_read(&fs, PWD_LIST_ID, &pwdList, sizeof(pwdList));
    if(numPwd == MAX_STORABLE_PWD){
        rc = -1;
    }else if(rc > 0 || numPwd == 0){
        /* List was found */
        int i = 0;
        while(i < numPwd){
            if(strcmp(pwdStruct->url, pwdList[i].url) == 0 && strcmp(pwdStruct->username, pwdList[i].username) == 0){
                /* Password previously stored. Update new password */
                printk("Updating new password...\n");
                strcpy(pwdList[i].pwd, pwdStruct->pwd);
                (void)nvs_write(&fs, PWD_LIST_ID, &pwdList, sizeof(pwdList));

                (void)nvs_write(&fs, NUM_PWD_ID, &numPwd, sizeof(numPwd));
                return 0;
            }else{
                i++;
            }
        }

        /* Password not found. Store new password */
        if(numPwd < MAX_STORABLE_PWD || numPwd == 0){
            printk("Storing new password...\n");
            strcpy(pwdList[numPwd].url, pwdStruct->url);
            strcpy(pwdList[numPwd].username, pwdStruct->username);
            strcpy(pwdList[numPwd].pwd, pwdStruct->pwd);
            (void)nvs_write(&fs, PWD_LIST_ID, &pwdList, sizeof(pwdList));

            numPwd++;
            (void)nvs_write(&fs, NUM_PWD_ID, &numPwd, sizeof(numPwd));
            return 0;
        }else{
            /* Reached MAX_STORABLE_PWD */
            rc = -1;
        }
    }else{
        strcpy(pwdStruct->pwd, "errno");
    }
    printk("numPwd: %d\n", numPwd);

    return rc;
}

void deleteAllPwd(){
    numPwd = 0;
	(void)nvs_write(&fs, NUM_PWD_ID, &numPwd, sizeof(numPwd));
}