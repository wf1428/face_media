var __defProp = Object.defineProperty;
var __getOwnPropDesc = Object.getOwnPropertyDescriptor;
var __getOwnPropNames = Object.getOwnPropertyNames;
var __hasOwnProp = Object.prototype.hasOwnProperty;
var __export = (target, all) => {
  for (var name in all)
    __defProp(target, name, { get: all[name], enumerable: true });
};
var __copyProps = (to, from, except, desc) => {
  if (from && typeof from === "object" || typeof from === "function") {
    for (let key of __getOwnPropNames(from))
      if (!__hasOwnProp.call(to, key) && key !== except)
        __defProp(to, key, { get: () => from[key], enumerable: !(desc = __getOwnPropDesc(from, key)) || desc.enumerable });
  }
  return to;
};
var __toCommonJS = (mod) => __copyProps(__defProp({}, "__esModule", { value: true }), mod);
var stdin_exports = {};
__export(stdin_exports, {
  default: () => stdin_default
});
module.exports = __toCommonJS(stdin_exports);
var stdin_default = {
  name: "Navn",
  tel: "Telefon",
  save: "Lagre",
  clear: "T\xF8m",
  undo: "Angre",
  cancel: "Avbryt",
  confirm: "Bekreft",
  delete: "Slett",
  loading: "Laster...",
  noCoupon: "Ingen kuponger",
  nameEmpty: "Vennligst fyll inn navn",
  addContact: "Legg til ny kontakt",
  telInvalid: "Ugyldig telefonnummer",
  vanCalendar: {
    end: "Slutt",
    start: "Start",
    title: "Kalender",
    weekdays: ["S\xF8n", "Man", "Tir", "Ons", "Tor", "Fre", "L\xF8r"],
    monthTitle: (year, month) => `${year}/${month}`,
    rangePrompt: (maxRange) => `Maks. ${maxRange} dager`
  },
  vanCascader: {
    select: "Velg"
  },
  vanPagination: {
    prev: "Forrige",
    next: "Neste"
  },
  vanPullRefresh: {
    pulling: "Dra for \xE5 oppdatere...",
    loosing: "Slipp for \xE5 oppdatere..."
  },
  vanSubmitBar: {
    label: "Totalt:"
  },
  vanCoupon: {
    unlimited: "Ubegrenset",
    discount: (discount) => `${discount * 10}% avslag`,
    condition: (condition) => `Minst ${condition}`
  },
  vanCouponCell: {
    title: "Kupong",
    count: (count) => `Du har ${count} kuponger`
  },
  vanCouponList: {
    exchange: "Bytt",
    close: "Lukk",
    enable: "Tilgjengelig",
    disabled: "Utilgjengelig",
    placeholder: "Kupongkode"
  },
  vanAddressEdit: {
    area: "Omr\xE5de",
    areaEmpty: "Vennligst velg omr\xE5de",
    addressEmpty: "Adressen kan ikke v\xE6re tom",
    addressDetail: "Adresse",
    defaultAddress: "Sett som standardadresse"
  },
  vanAddressList: {
    add: "Legg til ny adresse"
  }
};
