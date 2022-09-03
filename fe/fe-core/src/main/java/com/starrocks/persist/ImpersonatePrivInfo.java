// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

package com.starrocks.persist;

import com.google.gson.annotations.SerializedName;
import com.starrocks.analysis.UserIdentity;
import com.starrocks.common.io.Text;
import com.starrocks.common.io.Writable;
import com.starrocks.persist.gson.GsonUtils;

import java.io.DataInput;
import java.io.DataOutput;
import java.io.IOException;

public class ImpersonatePrivInfo implements Writable {
    @SerializedName(value = "authorizedUser")
    private UserIdentity authorizedUser = null;
    @SerializedName(value = "authorizedRoleName")
    private String authorizedRoleName = null;
    @SerializedName(value = "securedUser")
    private UserIdentity securedUser;

    /**
     * Empty constructor for gson
     */
    public ImpersonatePrivInfo() {}

    public ImpersonatePrivInfo(UserIdentity authorizedUser, UserIdentity securedUser) {
        this.authorizedUser = authorizedUser;
        this.securedUser = securedUser;
    }

    public ImpersonatePrivInfo(String authorizedRoleName, UserIdentity securedUser) {
        this.authorizedRoleName = authorizedRoleName;
        // Just in case of NPE after rolled back. Worst case scenario, it's meaningless to impersonate as oneself.
        this.authorizedUser = securedUser;
        this.securedUser = securedUser;
    }

    public UserIdentity getAuthorizedUser() {
        return authorizedUser;
    }

    public UserIdentity getSecuredUser() {
        return securedUser;
    }

    public String getAuthorizedRoleName() {
        return authorizedRoleName;
    }

    public static ImpersonatePrivInfo read(DataInput in) throws IOException {
        String json = Text.readString(in);
        return GsonUtils.GSON.fromJson(json, ImpersonatePrivInfo.class);
    }


    @Override
    public void write(DataOutput out) throws IOException {
        Text.writeString(out, GsonUtils.GSON.toJson(this));
    }
}
