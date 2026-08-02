#include "EdgeRepair.h"

typedef struct {
	A_u_long	index;
	A_char		str[256];
} TableString;

TableString		g_strs[StrID_NUMTYPES] = {
	StrID_NONE,			"",
	StrID_Name,			"Edge Repair",
	StrID_Description,	"Extends trusted interior color across contaminated matte edges along a Euclidean distance field, preserving plate texture. RGB in, RGB out; alpha untouched unless choked.",
};

char	*GetStringPtr(int strNum)
{
	return g_strs[strNum].str;
}
