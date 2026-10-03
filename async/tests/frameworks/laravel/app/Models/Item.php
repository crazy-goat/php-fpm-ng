<?php

namespace App\Models;

use Illuminate\Database\Eloquent\Model;

final class Item extends Model
{
    protected $table = 'probe_items';

    public $timestamps = false;

    protected $guarded = [];
}
