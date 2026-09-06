<?php

namespace App\Models;

use Illuminate\Foundation\Auth\User as Authenticatable;

final class User extends Authenticatable
{
    protected $table = 'users';

    public $timestamps = true;

    protected $guarded = [];

    protected $hidden = [
        'password',
        'remember_token',
    ];
}
